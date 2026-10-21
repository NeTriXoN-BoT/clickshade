#include "audio.h"

#include <pulse/pulseaudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <poll.h>
#include <thread>
#include <vector>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace clickshade::linuxapp {
namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned kRate = 48000;
constexpr unsigned kVoices = 12;
constexpr unsigned kKeyVariants = 6;
constexpr std::size_t kInitialFrames = kRate / 10; // First 100 ms, predecoded before a click.
constexpr std::size_t kSourceFrames = 4096; // At most 64 KiB per active voice.
// Queue slots: three mouse buttons, the key categories, then the key preview.
constexpr unsigned kMouseSlots = 3;
constexpr unsigned kKeyPreviewSlot = kMouseSlots + kKeyCategoryCount;
constexpr unsigned kQueueSlots = kKeyPreviewSlot + 1;
constexpr unsigned kKeyboardError = 3; // fileErrors index for the keyboard set.
struct FileDescriptor {
    explicit FileDescriptor(int descriptor) noexcept : value(descriptor) {}
    ~FileDescriptor() { if (value >= 0) close(value); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int value;
};
struct Clip {
    ~Clip() { if (file >= 0) close(file); }
    Clip() = default;
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;
    int file = -1;
    unsigned encoding = 0, channels = 0, rate = 0, block = 0, bits = 0;
    std::uint64_t dataOffset = 0, frames = 0, outputFrames = 0;
    double gain = 1;
    std::vector<std::int16_t> samples;
};
struct SourceWindow {
    std::array<unsigned char, kSourceFrames * 16> bytes{};
    std::uint64_t first = 0;
    std::size_t count = 0;
};
std::uint16_t U16(const unsigned char* p) { return p[0] | (std::uint16_t(p[1]) << 8); }
std::uint32_t U32(const unsigned char* p) { return U16(p) | (std::uint32_t(U16(p + 2)) << 16); }
bool Tag(const unsigned char* p, const char* s) { return std::memcmp(p, s, 4) == 0; }
bool ReadAt(int fd, void* destination, std::size_t length, std::uint64_t offset) {
    auto* bytes = static_cast<unsigned char*>(destination);
    std::size_t received = 0;
    while (received < length) {
        const auto count = pread(fd, bytes + received, length - received, static_cast<off_t>(offset + received));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        received += static_cast<std::size_t>(count);
    }
    return true;
}
bool SourceFrame(const Clip& clip, SourceWindow& source, std::uint64_t frame, double& left, double& right) {
    if (!source.count || frame < source.first || frame - source.first >= source.count) {
        source.first = frame;
        source.count = static_cast<std::size_t>(std::min<std::uint64_t>(kSourceFrames, clip.frames - frame));
        if (!ReadAt(clip.file, source.bytes.data(), source.count * clip.block, clip.dataOffset + frame * clip.block)) {
            source.count = 0;
            return false;
        }
    }
    const auto sample = [&](unsigned channel) {
        const auto* p = source.bytes.data() + (frame - source.first) * clip.block +
            std::min(channel, clip.channels - 1) * (clip.bits / 8);
        double value = 0;
        if (clip.encoding == 3) {
            if (clip.bits == 32) { const auto raw = U32(p); float v; std::memcpy(&v, &raw, sizeof(v)); value = v; }
            else { const std::uint64_t raw = U32(p) | (std::uint64_t(U32(p + 4)) << 32); std::memcpy(&value, &raw, sizeof(value)); }
        } else if (clip.bits == 8) value = (int(p[0]) - 128) / 128.0;
        else if (clip.bits == 16) value = static_cast<std::int16_t>(U16(p)) / 32768.0;
        else if (clip.bits == 24) {
            std::int32_t v = p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16);
            if (v & 0x800000) v |= static_cast<std::int32_t>(0xff000000u);
            value = v / 8388608.0;
        } else value = static_cast<std::int32_t>(U32(p)) / 2147483648.0;
        return std::isfinite(value) ? std::clamp(value, -1.0, 1.0) : 0.0;
    };
    left = sample(0); right = sample(1);
    return true;
}
bool OutputFrame(const Clip& clip, SourceWindow& source, std::uint64_t frame, std::int16_t& left, std::int16_t& right) {
    if (frame >= clip.outputFrames) return false;
    if (frame < clip.samples.size() / 2) {
        left = clip.samples[static_cast<std::size_t>(frame) * 2];
        right = clip.samples[static_cast<std::size_t>(frame) * 2 + 1];
        return true;
    }
    const double position = double(frame) * clip.rate / kRate;
    const auto first = std::min<std::uint64_t>(static_cast<std::uint64_t>(position), clip.frames - 1);
    const auto next = std::min(first + 1, clip.frames - 1);
    double aLeft, aRight, bLeft, bRight;
    if (!SourceFrame(clip, source, first, aLeft, aRight) || !SourceFrame(clip, source, next, bLeft, bRight)) return false;
    const double blend = position - double(first);
    const auto scale = [&](double a, double b) {
        return static_cast<std::int16_t>(std::lround(std::clamp((a * (1 - blend) + b * blend) * clip.gain, -1.0, 1.0) * 32767));
    };
    left = scale(aLeft, bLeft); right = scale(aRight, bRight);
    return true;
}

std::shared_ptr<const Clip> LoadWave(const std::string& path, int volume, std::string& error) {
    error.clear();
    if (path.empty()) return {};
    // A picker can choose a FIFO/device. Validate the same nonblocking-opened
    // descriptor used for all later reads; no path-swap or duration-sized buffer.
    FileDescriptor file(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY));
    if (file.value < 0) { error = "Could not open the sound file: " + path; return {}; }
    struct stat info{};
    if (fstat(file.value, &info) != 0) { error = "Could not inspect the sound file."; return {}; }
    if (!S_ISREG(info.st_mode)) {
        error = "Choose a regular WAV file; folders, devices and named pipes are not supported."; return {};
    }
    std::array<unsigned char, 12> header{};
    if (info.st_size < 12 || !ReadAt(file.value, header.data(), header.size(), 0)) {
        error = "The WAV file has a truncated RIFF header."; return {};
    }
    if (!Tag(header.data(), "RIFF") || !Tag(header.data() + 8, "WAVE")) {
        error = "Linux sound files must be PCM or floating-point RIFF WAV files."; return {};
    }
    const std::uint64_t riffEnd = std::uint64_t(U32(header.data() + 4)) + 8;
    if (riffEnd < 12 || riffEnd > static_cast<std::uint64_t>(info.st_size)) {
        error = "The WAV file has a truncated RIFF container."; return {};
    }
    std::array<unsigned char, 40> format{};
    std::uint64_t dataOffset = 0, dataBytes = 0;
    std::size_t formatBytes = 0;
    unsigned chunks = 0;
    for (std::uint64_t at = 12; at + 8 <= riffEnd && !(formatBytes && dataOffset);) {
        // This bounds pathological metadata traversal, not clip duration.
        if (++chunks > 4096) { error = "This WAV has too many metadata chunks."; return {}; }
        std::array<unsigned char, 8> chunk{};
        if (!ReadAt(file.value, chunk.data(), chunk.size(), at)) { error = "Could not read the WAV metadata."; return {}; }
        const std::uint64_t size = U32(chunk.data() + 4), begin = at + 8;
        if (size > riffEnd - begin) { error = "The WAV file contains a truncated chunk."; return {}; }
        if (Tag(chunk.data(), "fmt ") && !formatBytes) {
            formatBytes = static_cast<std::size_t>(std::min<std::uint64_t>(format.size(), size));
            if (!ReadAt(file.value, format.data(), formatBytes, begin)) { error = "Could not read the WAV format."; return {}; }
        } else if (Tag(chunk.data(), "data") && !dataOffset) { dataOffset = begin; dataBytes = size; }
        at = begin + size + (size & 1u);
    }
    if (formatBytes < 16 || !dataOffset || !dataBytes) {
        error = "The WAV file is missing its audio format or sample data."; return {};
    }
    unsigned encoding = U16(format.data());
    const unsigned channels = U16(format.data() + 2), rate = U32(format.data() + 4);
    const unsigned block = U16(format.data() + 12), bits = U16(format.data() + 14);
    if (encoding == 0xfffe) {
        static constexpr unsigned char tail[] = {0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
        if (formatBytes < 40 || U16(format.data() + 16) < 22 ||
            std::memcmp(format.data() + 28, tail, sizeof(tail)) != 0 ||
            (U32(format.data() + 24) != 1 && U32(format.data() + 24) != 3)) {
            error = "This extended WAV encoding is unsupported."; return {};
        }
        encoding = U32(format.data() + 24);
        const unsigned validBits = U16(format.data() + 18);
        if (validBits && validBits != bits) { error = "WAV samples with reduced valid-bit width are unsupported."; return {}; }
    }
    const bool pcm = encoding == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool floating = encoding == 3 && (bits == 32 || bits == 64);
    if ((!pcm && !floating) || (channels != 1 && channels != 2) || rate < 8000 || rate > 192000 ||
        block != channels * (bits / 8) || !block || dataBytes % block) {
        error = "Use a mono/stereo PCM or float WAV, 8–192 kHz."; return {};
    }
    auto clip = std::make_shared<Clip>();
    clip->file = file.value; file.value = -1;
    clip->encoding = encoding; clip->channels = channels; clip->rate = rate;
    clip->block = block; clip->bits = bits; clip->dataOffset = dataOffset;
    clip->frames = dataBytes / block;
    clip->outputFrames = (clip->frames * kRate + rate - 1) / rate;
    clip->gain = std::clamp(volume, 0, 100) / 100.0;
    const auto cachedFrames = static_cast<std::size_t>(std::min<std::uint64_t>(clip->outputFrames, kInitialFrames));
    std::vector<std::int16_t> initial(cachedFrames * 2);
    SourceWindow source;
    for (std::size_t frame = 0; frame < cachedFrames; ++frame) {
        if (!OutputFrame(*clip, source, frame, initial[frame * 2], initial[frame * 2 + 1])) {
            error = "Could not read the initial sound samples."; return {};
        }
    }
    clip->samples = std::move(initial);
    return clip;
}

struct AudioVoice {
    std::shared_ptr<const Clip> clip;
    std::uint64_t at = 0;
    unsigned button = 0; // 0–2 mouse buttons, kKeyboardError for key voices.
    bool keyboard = false;
    Clock::time_point queuedAt{};
    SourceWindow source;
    void Reset() noexcept { clip.reset(); at = 0; source.count = 0; keyboard = false; }
};
// Mouse and keyboard voices carry their own live gain. A muted group releases
// its voices instead of mixing silence for the rest of a long clip.
template<class ReportError>
std::size_t MixFrames(std::array<AudioVoice, kVoices>& voices, std::int16_t* output, std::size_t frames,
                     unsigned volume, unsigned keyboardVolume, ReportError reportError) {
    for (auto& voice : voices) if (voice.clip && !(voice.keyboard ? keyboardVolume : volume)) voice.Reset();
    if (!volume && !keyboardVolume) return 0;
    std::size_t used = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        long left = 0, right = 0; bool active = false;
        for (auto& voice : voices) if (voice.clip) {
            std::int16_t l = 0, r = 0;
            if (!OutputFrame(*voice.clip, voice.source, voice.at, l, r)) {
                const unsigned button = voice.button;
                voice.Reset(); reportError(button); continue;
            }
            const long gain = voice.keyboard ? keyboardVolume : volume;
            active = true; left += l * gain; right += r * gain;
            if (++voice.at >= voice.clip->outputFrames) voice.Reset();
        }
        if (!active) break;
        output[frame * 2] = static_cast<std::int16_t>(std::clamp(left / 100, -32768l, 32767l));
        output[frame * 2 + 1] = static_cast<std::int16_t>(std::clamp(right / 100, -32768l, 32767l));
        used = frame + 1;
    }
    return used;
}
template<class ReportError>
std::size_t MixFrames(std::array<AudioVoice, kVoices>& voices, std::int16_t* output,
                     std::size_t frames, unsigned volume, ReportError reportError) {
    return MixFrames(voices, output, frames, volume, volume, reportError);
}

struct PulseApi {
    PulseApi() = default;
    PulseApi(const PulseApi&) = delete;
    PulseApi& operator=(const PulseApi&) = delete;
    void* library = nullptr;
#define PULSE_FUNCTION(name) decltype(&name) name##_fn = nullptr
    PULSE_FUNCTION(pa_mainloop_new); PULSE_FUNCTION(pa_mainloop_free);
    PULSE_FUNCTION(pa_mainloop_get_api); PULSE_FUNCTION(pa_mainloop_set_poll_func);
    PULSE_FUNCTION(pa_mainloop_iterate); PULSE_FUNCTION(pa_context_new);
    PULSE_FUNCTION(pa_context_unref); PULSE_FUNCTION(pa_context_connect);
    PULSE_FUNCTION(pa_context_disconnect); PULSE_FUNCTION(pa_context_get_state);
    PULSE_FUNCTION(pa_context_set_state_callback); PULSE_FUNCTION(pa_context_errno);
    PULSE_FUNCTION(pa_strerror); PULSE_FUNCTION(pa_stream_new);
    PULSE_FUNCTION(pa_stream_unref); PULSE_FUNCTION(pa_stream_disconnect);
    PULSE_FUNCTION(pa_stream_connect_playback); PULSE_FUNCTION(pa_stream_set_state_callback);
    PULSE_FUNCTION(pa_stream_set_write_callback); PULSE_FUNCTION(pa_stream_get_state);
    PULSE_FUNCTION(pa_stream_writable_size); PULSE_FUNCTION(pa_stream_write);
    PULSE_FUNCTION(pa_stream_flush); PULSE_FUNCTION(pa_operation_unref);
#undef PULSE_FUNCTION
    ~PulseApi() { if (library) dlclose(library); }
    bool Load() {
        if (library && pa_operation_unref_fn) return true;
        if (library) { dlclose(library); library = nullptr; }
        library = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) return false;
#define LOAD(name) name##_fn = reinterpret_cast<decltype(name##_fn)>(dlsym(library, #name)); if (!name##_fn) return false
        LOAD(pa_mainloop_new); LOAD(pa_mainloop_free); LOAD(pa_mainloop_get_api);
        LOAD(pa_mainloop_set_poll_func); LOAD(pa_mainloop_iterate); LOAD(pa_context_new);
        LOAD(pa_context_unref); LOAD(pa_context_connect); LOAD(pa_context_disconnect);
        LOAD(pa_context_get_state); LOAD(pa_context_set_state_callback); LOAD(pa_context_errno);
        LOAD(pa_strerror); LOAD(pa_stream_new); LOAD(pa_stream_unref); LOAD(pa_stream_disconnect);
        LOAD(pa_stream_connect_playback); LOAD(pa_stream_set_state_callback);
        LOAD(pa_stream_set_write_callback); LOAD(pa_stream_get_state);
        LOAD(pa_stream_writable_size); LOAD(pa_stream_write); LOAD(pa_stream_flush);
        LOAD(pa_operation_unref);
#undef LOAD
        return true;
    }
};
std::string LowerAscii(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
} // namespace

struct LinuxAudio::Impl {
    std::atomic<bool> stopping{false}, running{false}, allowed{false}, enabled{false}, keyboardEnabled{false};
    std::atomic<bool> settingsPending{false}, clearPending{false};
    std::atomic<unsigned> volume{75}, keyboardVolume{60};
    std::array<std::atomic<unsigned>, kQueueSlots> queued{};
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    std::thread thread;
    mutable std::mutex mutex;
    Settings pending;
    std::string lastError;
    std::array<std::string, 4> fileErrors;
    std::string deviceError;
    std::array<std::shared_ptr<const Clip>, kMouseSlots> clips;
    std::vector<std::shared_ptr<const Clip>> keyVariants;
    std::array<std::shared_ptr<const Clip>, kKeyCategoryCount> keySpecial;
    unsigned lastVariant = 0;
    std::uint32_t variantSeed = 0x9e3779b9u;
    std::array<AudioVoice, kVoices> voices;
    PulseApi pulse;
    pa_mainloop* loop = nullptr;
    pa_context* context = nullptr;
    pa_stream* stream = nullptr;
    bool connectionFailed = false;
    Clock::time_point retryAfter{};

    ~Impl() { if (wake >= 0) close(wake); }
    void Wake() noexcept { const std::uint64_t one = 1; if (wake >= 0) { const auto ignored = write(wake, &one, sizeof(one)); (void)ignored; } }
    void SetError() {
        std::string value;
        const auto append = [&](const char* prefix, const std::string& text) {
            if (text.empty()) return;
            if (!value.empty()) value += "\n";
            value += prefix; value += text;
        };
        // A file problem only matters while its sounds are switched on;
        // turning them on reloads the files and reports it then.
        if (enabled.load(std::memory_order_relaxed)) {
            append("Left sound: ", fileErrors[0]);
            append("Right sound: ", fileErrors[1]);
            append("Middle sound: ", fileErrors[2]);
        }
        if (keyboardEnabled.load(std::memory_order_relaxed)) append("Keyboard sounds: ", fileErrors[kKeyboardError]);
        append("", deviceError);
        std::lock_guard<std::mutex> lock(mutex); lastError = std::move(value);
    }
    void EnqueueSlot(unsigned slot) noexcept {
        auto count = queued[slot].load(std::memory_order_relaxed);
        for (unsigned tries = 0; tries < 3 && count < kVoices; ++tries)
            if (queued[slot].compare_exchange_weak(count, count + 1, std::memory_order_release, std::memory_order_relaxed)) break;
    }
    void Enqueue(unsigned mask) noexcept {
        if (!running.load(std::memory_order_relaxed) || !allowed.load(std::memory_order_relaxed) || !volume.load(std::memory_order_relaxed)) return;
        for (unsigned i = 0; i < kMouseSlots; ++i) if (mask & (1u << i)) EnqueueSlot(i);
        Wake();
    }
    void EnqueueKey(unsigned slot) noexcept {
        if (!running.load(std::memory_order_relaxed) || !allowed.load(std::memory_order_relaxed) || !keyboardVolume.load(std::memory_order_relaxed)) return;
        EnqueueSlot(slot);
        Wake();
    }
    static int Poll(struct pollfd* fds, unsigned long count, int timeout, void* userdata) {
        auto& self = *static_cast<Impl*>(userdata);
        std::vector<pollfd> watched(count + 1);
        if (count) std::copy_n(fds, count, watched.begin());
        watched.back() = {self.wake, POLLIN, 0};
        const int result = poll(watched.data(), watched.size(), timeout);
        if (result < 0) return result;
        int ready = 0;
        for (unsigned long i = 0; i < count; ++i) { fds[i].revents = watched[i].revents; if (fds[i].revents) ++ready; }
        if (watched.back().revents) {
            std::uint64_t counter;
            while (read(self.wake, &counter, sizeof(counter)) == sizeof(counter)) {}
        }
        return ready;
    }
    void CloseConnection() {
        if (stream) { pulse.pa_stream_set_write_callback_fn(stream, nullptr, nullptr); pulse.pa_stream_set_state_callback_fn(stream, nullptr, nullptr); pulse.pa_stream_disconnect_fn(stream); pulse.pa_stream_unref_fn(stream); stream = nullptr; }
        if (context) { pulse.pa_context_set_state_callback_fn(context, nullptr, nullptr); pulse.pa_context_disconnect_fn(context); pulse.pa_context_unref_fn(context); context = nullptr; }
    }
    void DeviceFailure(const char* prefix) {
        deviceError = prefix;
        if (context) deviceError += std::string(": ") + pulse.pa_strerror_fn(pulse.pa_context_errno_fn(context));
        connectionFailed = true;
        retryAfter = Clock::now() + std::chrono::seconds(2);
        SetError();
    }
    static void ContextChanged(pa_context* context, void* userdata) {
        auto& self = *static_cast<Impl*>(userdata);
        switch (self.pulse.pa_context_get_state_fn(context)) {
        case PA_CONTEXT_READY: {
            pa_sample_spec format{PA_SAMPLE_S16LE, kRate, 2};
            self.stream = self.pulse.pa_stream_new_fn(context, "Click and key sounds", &format, nullptr);
            if (!self.stream) { self.DeviceFailure("Could not create the sound stream"); return; }
            self.pulse.pa_stream_set_state_callback_fn(self.stream, StreamChanged, &self);
            self.pulse.pa_stream_set_write_callback_fn(self.stream, WriteReady, &self);
            pa_buffer_attr buffer{19200, 3840, 0, 960, static_cast<std::uint32_t>(-1)};
            if (self.pulse.pa_stream_connect_playback_fn(self.stream, nullptr, &buffer, PA_STREAM_ADJUST_LATENCY, nullptr, nullptr) < 0)
                self.DeviceFailure("Could not start the sound stream");
            break;
        }
        case PA_CONTEXT_FAILED: case PA_CONTEXT_TERMINATED:
            self.DeviceFailure("PulseAudio/PipeWire sound output is unavailable"); break;
        default: break;
        }
    }
    static void StreamChanged(pa_stream* stream, void* userdata) {
        auto& self = *static_cast<Impl*>(userdata);
        const auto state = self.pulse.pa_stream_get_state_fn(stream);
        if (state == PA_STREAM_READY) { self.deviceError.clear(); self.SetError(); self.Feed(); }
        else if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) self.DeviceFailure("The sound stream stopped");
    }
    static void WriteReady(pa_stream*, std::size_t, void* userdata) { static_cast<Impl*>(userdata)->Feed(); }
    void EnsureConnection() {
        if (context || !loop || Clock::now() < retryAfter) return;
        context = pulse.pa_context_new_fn(pulse.pa_mainloop_get_api_fn(loop), "ClickShade");
        if (!context) { DeviceFailure("Could not create the sound connection"); return; }
        pulse.pa_context_set_state_callback_fn(context, ContextChanged, this);
        if (pulse.pa_context_connect_fn(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
            DeviceFailure("PulseAudio/PipeWire sound output is unavailable");
    }
    void Clear() {
        for (auto& q : queued) q.store(0, std::memory_order_relaxed);
        for (auto& voice : voices) voice.Reset();
        if (stream && pulse.pa_stream_get_state_fn(stream) == PA_STREAM_READY) {
            if (auto* op = pulse.pa_stream_flush_fn(stream, nullptr, nullptr)) pulse.pa_operation_unref_fn(op);
        }
    }
    void Feed() {
        if (!allowed.load(std::memory_order_relaxed) || !stream || pulse.pa_stream_get_state_fn(stream) != PA_STREAM_READY) return;
        const auto now = Clock::now();
        for (auto& voice : voices) if (voice.clip && !voice.at && now - voice.queuedAt > std::chrono::milliseconds(250)) voice.Reset();
        std::size_t writable = pulse.pa_stream_writable_size_fn(stream);
        if (writable == static_cast<std::size_t>(-1)) return;
        std::array<std::int16_t, 960> output{};
        // The stream's requested buffer is bounded to 100 ms; no unbounded queue.
        while (writable >= 4) {
            const std::size_t frames = std::min<std::size_t>(output.size() / 2, writable / 4);
            const auto used = MixFrames(voices, output.data(), frames, volume.load(std::memory_order_relaxed),
                                        keyboardVolume.load(std::memory_order_relaxed), [this](unsigned button) {
                fileErrors[std::min(button, kKeyboardError)] = "The sound file changed or became unreadable during playback.";
                SetError();
            });
            if (!used) break;
            if (pulse.pa_stream_write_fn(stream, output.data(), used * 4, nullptr, 0, PA_SEEK_RELATIVE) < 0) { DeviceFailure("Could not write sound samples"); break; }
            writable -= used * 4;
        }
    }
    // A set folder holds key*.wav variants (up to six) with optional
    // space/enter/backspace files; a single WAV serves every key.
    void LoadKeyboardSet(const std::string& path) {
        keyVariants.clear(); keySpecial = {}; lastVariant = 0;
        auto& error = fileErrors[kKeyboardError];
        error.clear();
        if (path.empty()) return;
        struct stat info{};
        if (stat(path.c_str(), &info) != 0) { error = "Could not open the keyboard sound set: " + path; return; }
        if (!S_ISDIR(info.st_mode)) {
            if (auto clip = LoadWave(path, 100, error)) keyVariants.push_back(std::move(clip));
            return;
        }
        std::vector<std::string> keys;
        std::array<std::string, kKeyCategoryCount> special{};
        std::error_code listing;
        unsigned inspected = 0;
        for (std::filesystem::directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, listing), end;
             !listing && it != end && inspected < 512; it.increment(listing), ++inspected) {
            std::error_code type;
            if (!it->is_regular_file(type) || type) continue;
            const auto name = LowerAscii(it->path().filename().string());
            if (name.size() < 5 || name.compare(name.size() - 4, 4, ".wav") != 0) continue;
            const auto stem = name.substr(0, name.size() - 4);
            const auto full = it->path().string();
            if (stem == "space") special[kKeySpace] = full;
            else if (stem == "enter" || stem == "return") special[kKeyEnter] = full;
            else if (stem == "backspace") special[kKeyBackspace] = full;
            else if (stem.rfind("key", 0) == 0) keys.push_back(full);
        }
        std::sort(keys.begin(), keys.end());
        std::string first;
        for (unsigned n = 0; n < keys.size() && keyVariants.size() < kKeyVariants; ++n) {
            std::string problem;
            if (auto clip = LoadWave(keys[n], 100, problem)) keyVariants.push_back(std::move(clip));
            else if (first.empty()) first = problem;
        }
        for (unsigned category = kKeySpace; category < kKeyCategoryCount; ++category) {
            if (special[category].empty()) continue;
            std::string problem;
            keySpecial[category] = LoadWave(special[category], 100, problem);
            if (!problem.empty() && first.empty()) first = problem;
        }
        if (keys.empty()) error = "The keyboard sound folder has no key*.wav file. Choose a set from sounds/keyboard or a single WAV.";
        else if (keyVariants.empty()) error = first;
    }
    std::shared_ptr<const Clip> KeyClip(unsigned category) {
        if (category < kKeyCategoryCount && keySpecial[category]) return keySpecial[category];
        if (keyVariants.empty()) return {};
        if (keyVariants.size() == 1) return keyVariants.front();
        // Never repeat the previous variant so fast typing does not machine-gun one sample.
        variantSeed = variantSeed * 1664525u + 1013904223u;
        unsigned variant = (variantSeed >> 16) % static_cast<unsigned>(keyVariants.size() - 1);
        if (variant >= lastVariant) ++variant;
        lastVariant = variant;
        return keyVariants[variant];
    }
    bool AddVoice(const std::shared_ptr<const Clip>& clip, unsigned button, bool keyboard) {
        if (!clip) return false;
        auto empty = std::find_if(voices.begin(), voices.end(), [](const AudioVoice& v) { return !v.clip; });
        if (empty == voices.end()) return false;
        empty->Reset(); empty->clip = clip; empty->button = button; empty->keyboard = keyboard;
        empty->queuedAt = Clock::now();
        return true;
    }
    void Commands() {
        if (clearPending.exchange(false, std::memory_order_acquire)) Clear();
        if (settingsPending.exchange(false, std::memory_order_acquire)) {
            Settings next;
            { std::lock_guard<std::mutex> lock(mutex); next = pending; }
            Clear();
            clips[0] = LoadWave(next.leftSoundPath, 100, fileErrors[0]);
            clips[1] = LoadWave(next.rightSoundPath, 100, fileErrors[1]);
            clips[2] = LoadWave(next.middleSoundPath, 100, fileErrors[2]);
            LoadKeyboardSet(next.keyboardSoundPath);
            if (loop) deviceError.clear();
            SetError();
            const bool mouseReady = next.clickSoundsEnabled && (clips[0] || clips[1] || clips[2]);
            const bool keyboardReady = next.keyboardSoundsEnabled && !keyVariants.empty();
            if (mouseReady || keyboardReady) EnsureConnection();
        }
        if (!allowed.load(std::memory_order_relaxed)) return;
        if (!context && Clock::now() < retryAfter) {
            for (auto& q : queued) q.store(0, std::memory_order_relaxed);
            return; // Failed output must not accumulate old clicks for recovery.
        }
        bool added = false;
        for (unsigned slot = 0; slot < kQueueSlots; ++slot) {
            const auto count = std::min(queued[slot].exchange(0, std::memory_order_acquire), kVoices);
            for (unsigned n = 0; n < count; ++n) {
                bool ok = false;
                if (slot < kMouseSlots) ok = AddVoice(clips[slot], slot, false);
                else if (slot == kKeyPreviewSlot) ok = AddVoice(KeyClip(kKeyGeneric), kKeyboardError, true);
                else ok = AddVoice(KeyClip(slot - kMouseSlots), kKeyboardError, true);
                if (!ok) break;
                added = true;
            }
        }
        if (added) EnsureConnection();
        Feed();
    }
    void Run() {
        if (pulse.Load()) {
            loop = pulse.pa_mainloop_new_fn();
            if (loop) pulse.pa_mainloop_set_poll_func_fn(loop, Poll, this);
        }
        if (!loop) { deviceError = "Optional libpulse sound output is unavailable."; SetError(); }
        while (!stopping.load(std::memory_order_acquire)) {
            Commands();
            if (connectionFailed) { CloseConnection(); connectionFailed = false; for (auto& v : voices) v.Reset(); }
            if (stopping.load(std::memory_order_acquire)) break;
            if (loop) {
                int result = 0;
                if (pulse.pa_mainloop_iterate_fn(loop, 1, &result) < 0) break;
            } else {
                pollfd fd{wake, POLLIN, 0};
                if (poll(&fd, 1, -1) > 0) { std::uint64_t n; while (read(wake, &n, sizeof(n)) == sizeof(n)) {} }
            }
        }
        Clear(); CloseConnection();
        if (loop) { pulse.pa_mainloop_free_fn(loop); loop = nullptr; }
        running.store(false, std::memory_order_release);
    }
};

LinuxAudio::LinuxAudio() : impl_(std::make_unique<Impl>()) {}
LinuxAudio::~LinuxAudio() { Stop(); }
bool LinuxAudio::Start(const Settings& settings, std::string& error) {
    Stop(); error.clear();
    if (impl_->wake < 0) { error = "Could not create the sound worker wake event."; return false; }
    impl_->stopping.store(false); impl_->allowed.store(false);
    impl_->running.store(true);
    Update(settings);
    impl_->settingsPending.store(true, std::memory_order_release);
    try { impl_->thread = std::thread([this] { try { impl_->Run(); } catch (...) { impl_->deviceError = "The optional sound worker stopped unexpectedly."; impl_->SetError(); impl_->CloseConnection(); if (impl_->loop) { impl_->pulse.pa_mainloop_free_fn(impl_->loop); impl_->loop = nullptr; } impl_->running.store(false); } }); }
    catch (const std::exception& e) { impl_->running.store(false); error = e.what(); return false; }
    return true;
}
void LinuxAudio::Stop() noexcept {
    impl_->allowed.store(false); impl_->stopping.store(true); impl_->Wake();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->running.store(false);
}
void LinuxAudio::Update(const Settings& settings) {
    impl_->enabled.store(settings.clickSoundsEnabled, std::memory_order_relaxed);
    impl_->keyboardEnabled.store(settings.keyboardSoundsEnabled, std::memory_order_relaxed);
    const unsigned volume = static_cast<unsigned>(std::clamp(settings.soundVolume, 0, 100));
    const unsigned keyboardVolume = static_cast<unsigned>(std::clamp(settings.keyboardVolume, 0, 100));
    const bool gainChanged = impl_->volume.exchange(volume, std::memory_order_relaxed) != volume;
    const bool keyGainChanged = impl_->keyboardVolume.exchange(keyboardVolume, std::memory_order_relaxed) != keyboardVolume;
    if ((gainChanged && !volume) || (keyGainChanged && !keyboardVolume)) impl_->clearPending.store(true, std::memory_order_release);
    bool rebuild = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        rebuild = impl_->pending.leftSoundPath != settings.leftSoundPath || impl_->pending.rightSoundPath != settings.rightSoundPath ||
            impl_->pending.middleSoundPath != settings.middleSoundPath || impl_->pending.clickSoundsEnabled != settings.clickSoundsEnabled ||
            impl_->pending.keyboardSoundPath != settings.keyboardSoundPath || impl_->pending.keyboardSoundsEnabled != settings.keyboardSoundsEnabled;
        if (rebuild) impl_->pending = settings;
        else { impl_->pending.soundVolume = settings.soundVolume; impl_->pending.keyboardVolume = settings.keyboardVolume; }
    }
    // Volume-only edits apply to the next mixed buffer of existing voices.
    // They retain playback positions and never reopen or decode selected files.
    if (rebuild) impl_->settingsPending.store(true, std::memory_order_release);
    if (rebuild || gainChanged || keyGainChanged) impl_->Wake();
}
void LinuxAudio::SetAllowed(bool value) noexcept {
    if (impl_->allowed.exchange(value, std::memory_order_acq_rel) == value) return;
    if (!value) impl_->clearPending.store(true, std::memory_order_release);
    impl_->Wake();
}
void LinuxAudio::Click(unsigned mask) noexcept { if (impl_->enabled.load(std::memory_order_relaxed)) impl_->Enqueue(mask & 7u); }
void LinuxAudio::Key(unsigned category) noexcept {
    if (!impl_->keyboardEnabled.load(std::memory_order_relaxed)) return;
    impl_->EnqueueKey(kMouseSlots + std::min(category, kKeyCategoryCount - 1u));
}
void LinuxAudio::Preview(unsigned mask) noexcept { impl_->Enqueue(mask & 7u); }
void LinuxAudio::PreviewKey() noexcept { impl_->EnqueueKey(kKeyPreviewSlot); }
std::string LinuxAudio::LastError() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->lastError; }

} // namespace clickshade::linuxapp
