#include "click_audio.h"

#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace clickshade {
namespace {
constexpr DWORD kOutputRate = 48000;
constexpr unsigned kMaximumVoices = 16;
constexpr unsigned kMaximumQueuedPerRequest = 8;
constexpr std::size_t kInitialFrames = kOutputRate / 10;   // 100 ms prepared prefix per clip.
constexpr std::size_t kSourceFrames = 4096;                // 64 KiB source window per voice.
constexpr std::size_t kPrefetchFrames = kOutputRate / 10;  // 100 ms decoded ring per voice.
constexpr std::size_t kMixFrames = kOutputRate / 100;      // 10 ms mixing chunk.
constexpr std::size_t kMaximumPacketBytes = 1024u * 1024u;
constexpr unsigned kKeyVariants = 6;
constexpr DWORD kDeviceIdleCloseMs = 20000;
constexpr DWORD kDeviceRetryMs = 1500;

// Clip slots. Keyboard variants are chosen round-robin per key press.
enum Slot : unsigned {
    kSlotLeft = 0, kSlotRight, kSlotMiddle,
    kSlotKeyFirst, kSlotKeyLast = kSlotKeyFirst + kKeyVariants - 1,
    kSlotSpace, kSlotEnter, kSlotBackspace, kSlotCount
};
// Bounded request counters written by the input thread.
enum Request : unsigned {
    kRequestLeft = 0, kRequestRight, kRequestMiddle,
    kRequestKey, kRequestSpace, kRequestEnter, kRequestBackspace,
    kRequestPreviewLeft, kRequestPreviewRight, kRequestPreviewMiddle, kRequestPreviewKey,
    kRequestCount
};
enum class Group : unsigned { Mouse, Keyboard };

struct AudioOptions {
    bool mouseEnabled = false;
    bool keyboardEnabled = false;
    std::wstring left, right, middle, keyboard;
    int mouseVolume = 75;
    int keyboardVolume = 60;
};

AudioOptions OptionsFrom(const Settings& settings) {
    AudioOptions options;
    options.mouseEnabled = settings.clickSoundsEnabled;
    options.keyboardEnabled = settings.keyboardSoundsEnabled;
    options.left = settings.leftSoundPath;
    options.right = settings.rightSoundPath;
    options.middle = settings.middleSoundPath;
    options.keyboard = settings.keyboardSoundPath;
    options.mouseVolume = std::clamp(settings.soundVolume, 0, 100);
    options.keyboardVolume = std::clamp(settings.keyboardVolume, 0, 100);
    return options;
}

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    FileHandle() = default;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

template<class T> class ComPtr {
public:
    ~ComPtr() { if (value_) value_->Release(); }
    T** Put() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void Reset() { if (value_) value_->Release(); value_ = nullptr; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
private:
    T* value_ = nullptr;
};

template<class Function> Function Procedure(HMODULE module, const char* name) {
    const FARPROC address = GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address), "Windows function pointer size");
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

std::uint16_t U16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}
std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::wstring HResultText(const wchar_t* action, HRESULT value) {
    wchar_t code[32]{};
    std::swprintf(code, std::size(code), L" (0x%08lX)", static_cast<unsigned long>(value));
    return std::wstring(action) + code;
}

std::wstring WaveError(MMRESULT result) {
    wchar_t text[256]{};
    if (waveOutGetErrorTextW(result, text, static_cast<UINT>(std::size(text))) == MMSYSERR_NOERROR)
        return std::wstring(L"Audio output: ") + text;
    return L"Could not open an audio output device (error " + std::to_wstring(result) + L").";
}

bool SupportedExtension(const std::wstring& path) {
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || path.find_first_of(L"\\/", dot) != std::wstring::npos) return false;
    const std::wstring extension = path.substr(dot);
    for (const wchar_t* candidate : {L".wav", L".mp3", L".wma", L".flac", L".m4a", L".aac"})
        if (_wcsicmp(extension.c_str(), candidate) == 0) return true;
    return false;
}

struct PcmFormat {
    unsigned channels = 0;
    unsigned rate = 0;
    unsigned bits = 0;
    bool floating = false;
};

struct Clip {
    // Only the first 100 ms is predecoded; duration never sizes an allocation.
    std::vector<std::int16_t> samples;
    std::shared_ptr<FileHandle> file;
    std::wstring path;
    PcmFormat format;
    std::uint64_t dataOffset = 0, frames = 0, outputFrames = 0;
    bool media = false, complete = false;
};
using ClipPtr = std::shared_ptr<const Clip>;
struct SourceWindow {
    std::array<std::uint8_t, kSourceFrames * 16> bytes{};
    std::uint64_t first = 0;
    std::size_t count = 0;
};

bool ReadAt(HANDLE file, void* destination, std::size_t bytes, std::uint64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD received = 0;
    // All file operations are serialized on the audio worker. Multiple voices
    // can share the validated handle without sharing a decoder's position.
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
        ReadFile(file, destination, static_cast<DWORD>(bytes), &received, nullptr) && received == bytes;
}

bool ValidFormat(const PcmFormat& format, std::wstring& error) {
    if (format.channels < 1 || format.channels > 2) {
        error = L"Choose a mono or stereo sound file.";
        return false;
    }
    if (format.rate < 8000 || format.rate > 192000) {
        error = L"The sound's sample rate must be between 8 kHz and 192 kHz.";
        return false;
    }
    if ((!format.floating && format.bits != 8 && format.bits != 16 &&
         format.bits != 24 && format.bits != 32) ||
        (format.floating && format.bits != 32 && format.bits != 64)) {
        error = L"The file uses an unsupported PCM sample format.";
        return false;
    }
    return true;
}

double Sample(const std::uint8_t* p, const PcmFormat& format) {
    double value = 0.0;
    if (format.floating && format.bits == 32) {
        float data = 0.0f;
        std::memcpy(&data, p, sizeof(data)); value = data;
    } else if (format.floating) {
        std::memcpy(&value, p, sizeof(value));
    } else if (format.bits == 8) {
        value = (static_cast<int>(p[0]) - 128) / 128.0;
    } else if (format.bits == 16) {
        value = static_cast<std::int16_t>(U16(p)) / 32768.0;
    } else if (format.bits == 24) {
        std::int32_t integer = static_cast<std::int32_t>(
            static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8) |
            (static_cast<unsigned>(p[2]) << 16));
        if (integer & 0x00800000) integer -= 0x01000000;
        value = integer / 8388608.0;
    } else {
        value = static_cast<std::int32_t>(U32(p)) / 2147483648.0;
    }
    return std::isfinite(value) ? std::clamp(value, -1.0, 1.0) : 0.0;
}

bool SourceFrame(const Clip& clip, SourceWindow& source, std::uint64_t frame,
                 double& left, double& right) {
    const unsigned stride = clip.format.channels * (clip.format.bits / 8);
    if (frame >= clip.frames) return false;
    if (!source.count || frame < source.first || frame - source.first >= source.count) {
        source.first = frame;
        source.count = static_cast<std::size_t>(std::min<std::uint64_t>(kSourceFrames, clip.frames - frame));
        if (!ReadAt(clip.file->value, source.bytes.data(), source.count * stride,
                    clip.dataOffset + frame * stride)) {
            source.count = 0; return false;
        }
    }
    const auto* bytes = source.bytes.data() + (frame - source.first) * stride;
    left = Sample(bytes, clip.format);
    right = Sample(bytes + (clip.format.channels == 1 ? 0 : clip.format.bits / 8), clip.format);
    return true;
}

bool OutputFrame(const Clip& clip, SourceWindow& source, std::uint64_t frame,
                 std::int16_t& left, std::int16_t& right) {
    if (frame >= clip.outputFrames) return false;
    if (frame < clip.samples.size() / 2) {
        left = clip.samples[static_cast<std::size_t>(frame) * 2];
        right = clip.samples[static_cast<std::size_t>(frame) * 2 + 1];
        return true;
    }
    const double position = static_cast<double>(frame) * clip.format.rate / kOutputRate;
    const auto first = std::min<std::uint64_t>(static_cast<std::uint64_t>(position), clip.frames - 1);
    const auto next = std::min(first + 1, clip.frames - 1);
    double aLeft, aRight, bLeft, bRight;
    if (!SourceFrame(clip, source, first, aLeft, aRight) || !SourceFrame(clip, source, next, bLeft, bRight)) return false;
    const double fraction = position - static_cast<double>(first);
    const auto convert = [&](double a, double b) {
        return static_cast<std::int16_t>(std::lround(std::clamp(a + (b - a) * fraction, -1.0, 1.0) * 32767.0));
    };
    left = convert(aLeft, bLeft); right = convert(aRight, bRight);
    return true;
}

// Parses bounded metadata from the same handle later used for samples. A
// recognized malformed WAV is rejected without handing it to another parser.
bool DecodeWave(const std::shared_ptr<FileHandle>& file, std::uint64_t fileSize,
                ClipPtr& output, bool& allowFallback, std::wstring& error) {
    allowFallback = false;
    std::array<std::uint8_t, 12> header{};
    if (fileSize < header.size() || !ReadAt(file->value, header.data(), header.size(), 0) ||
        std::memcmp(header.data(), "RIFF", 4) || std::memcmp(header.data() + 8, "WAVE", 4)) return false;
    const std::uint64_t declared = static_cast<std::uint64_t>(U32(header.data() + 4)) + 8;
    if (declared < 12 || declared > fileSize) {
        error = L"The WAV file is truncated or has an invalid RIFF header."; return true;
    }
    std::array<std::uint8_t, 40> formatBytes{};
    std::size_t formatSize = 0;
    std::uint64_t dataOffset = 0, audioSize = 0;
    unsigned chunks = 0;
    for (std::uint64_t position = 12; position + 8 <= declared && !(formatSize && dataOffset);) {
        if (++chunks > 4096) { error = L"The WAV file contains too many metadata chunks."; return true; }
        std::array<std::uint8_t, 8> chunk{};
        if (!ReadAt(file->value, chunk.data(), chunk.size(), position)) {
            error = L"The WAV metadata could not be read."; return true;
        }
        const std::uint64_t count = U32(chunk.data() + 4);
        position += 8;
        if (count > declared - position) { error = L"The WAV file contains a truncated chunk."; return true; }
        if (!formatSize && std::memcmp(chunk.data(), "fmt ", 4) == 0) {
            formatSize = static_cast<std::size_t>(std::min<std::uint64_t>(count, formatBytes.size()));
            if (!ReadAt(file->value, formatBytes.data(), formatSize, position)) {
                error = L"The WAV format could not be read."; return true;
            }
        } else if (!dataOffset && std::memcmp(chunk.data(), "data", 4) == 0) {
            dataOffset = position; audioSize = count;
        }
        position += count + (count & 1u);
    }
    if (formatSize < 16 || !dataOffset || !audioSize) {
        error = L"The WAV file is missing its format or audio data."; return true;
    }
    unsigned tag = U16(formatBytes.data());
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        constexpr std::uint8_t suffix[12] = {0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
        if (formatSize < 40 || U16(formatBytes.data() + 16) < 22 ||
            std::memcmp(formatBytes.data() + 28, suffix, sizeof(suffix))) {
            error = L"The WAV file has an unsupported extensible format."; return true;
        }
        tag = U32(formatBytes.data() + 24);
    }
    if (tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_IEEE_FLOAT) { allowFallback = true; return true; }
    auto clip = std::make_shared<Clip>();
    clip->format = {U16(formatBytes.data() + 2), U32(formatBytes.data() + 4),
                    U16(formatBytes.data() + 14), tag == WAVE_FORMAT_IEEE_FLOAT};
    if (!ValidFormat(clip->format, error)) return true;
    const unsigned stride = clip->format.channels * (clip->format.bits / 8);
    if (U16(formatBytes.data() + 12) != stride || audioSize % stride) {
        error = L"The WAV file has an invalid audio frame size."; return true;
    }
    clip->file = file;
    clip->dataOffset = dataOffset;
    clip->frames = audioSize / stride;
    clip->outputFrames = (clip->frames * kOutputRate + clip->format.rate - 1) / clip->format.rate;
    const auto initial = static_cast<std::size_t>(std::min<std::uint64_t>(clip->outputFrames, kInitialFrames));
    SourceWindow source;
    // Fill before publishing the prefix so OutputFrame reads original samples.
    std::vector<std::int16_t> prefix(initial * 2);
    for (std::size_t frame = 0; frame < initial; ++frame)
        if (!OutputFrame(*clip, source, frame, prefix[frame * 2], prefix[frame * 2 + 1])) {
            error = L"The WAV samples could not be read."; return true;
        }
    clip->samples = std::move(prefix);
    clip->complete = clip->outputFrames <= initial;
    output = std::move(clip);
    return true;
}

class MediaStream {
public:
    ComPtr<IMFSourceReader> reader;
    std::size_t Read(std::int16_t* destination, std::size_t frames,
                     const std::atomic<bool>& cancelled, std::wstring& error) {
        std::size_t copied = 0;
        unsigned emptyPackets = 0;
        while (copied < frames && !cancelled.load()) {
            if (position_ < packet_.size()) {
                const std::size_t count = std::min(frames - copied, (packet_.size() - position_) / 4);
                std::memcpy(destination + copied * 2, packet_.data() + position_, count * 4);
                position_ += count * 4; copied += count;
                continue;
            }
            if (ended_) break;
            if (++emptyPackets > 4096) {
                error = L"The audio decoder stopped making progress."; break;
            }
            DWORD flags = 0;
            ComPtr<IMFSample> sample;
            HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                            nullptr, &flags, nullptr, sample.Put());
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))) {
                error = HResultText(L"The audio decoder could not continue", FAILED(hr) ? hr : E_FAIL); break;
            }
            ended_ = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            packet_.clear(); position_ = 0;
            if (!sample.Get()) continue;
            DWORD total = 0;
            hr = sample->GetTotalLength(&total);
            if (FAILED(hr) || total > kMaximumPacketBytes || total % 4) {
                error = L"The audio decoder returned an invalid or oversized packet."; break;
            }
            if (!total) continue;
            ComPtr<IMFMediaBuffer> buffer;
            hr = sample->ConvertToContiguousBuffer(buffer.Put());
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, &length);
            if (FAILED(hr)) { error = HResultText(L"Could not read decoded audio", hr); break; }
            const bool valid = length == total && length <= kMaximumPacketBytes;
            try { if (valid) packet_.assign(data, data + length); }
            catch (...) { buffer->Unlock(); throw; }
            buffer->Unlock();
            if (!valid) { error = L"The audio decoder returned an inconsistent packet."; break; }
            emptyPackets = 0;
        }
        if (!error.empty()) ended_ = true;
        return copied;
    }
    bool Finished() const noexcept { return ended_ && position_ >= packet_.size(); }
private:
    std::vector<std::uint8_t> packet_;
    std::size_t position_ = 0;
    bool ended_ = false;
};

class MediaDecoder {
public:
    MediaDecoder() = default;
    MediaDecoder(const MediaDecoder&) = delete;
    MediaDecoder& operator=(const MediaDecoder&) = delete;
    ~MediaDecoder() {
        if (started_ && shutdown_) shutdown_();
        if (comInitialized_) CoUninitialize();
        if (readerLibrary_) FreeLibrary(readerLibrary_);
        if (platformLibrary_) FreeLibrary(platformLibrary_);
    }
    std::unique_ptr<MediaStream> Open(const std::wstring& path, std::wstring& error) {
        if (!Initialize(error)) return {};
        auto stream = std::make_unique<MediaStream>();
        HRESULT hr = createReader_(path.c_str(), nullptr, stream->reader.Put());
        if (FAILED(hr)) {
            error = HResultText(L"Windows could not decode this file. Try a PCM WAV file", hr); return {};
        }
        hr = stream->reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (SUCCEEDED(hr)) hr = stream->reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        ComPtr<IMFMediaType> requested;
        if (SUCCEEDED(hr)) hr = createType_(requested.Put());
        if (SUCCEEDED(hr)) hr = requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(hr)) hr = requested->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(hr)) hr = requested->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kOutputRate);
        if (SUCCEEDED(hr)) hr = requested->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = requested->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        if (SUCCEEDED(hr)) hr = requested->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kOutputRate * 4);
        if (SUCCEEDED(hr)) hr = stream->reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, requested.Get());
        ComPtr<IMFMediaType> actual;
        if (SUCCEEDED(hr)) hr = stream->reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, actual.Put());
        UINT32 channels = 0, rate = 0, bits = 0, alignment = 0;
        if (SUCCEEDED(hr)) hr = actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        if (SUCCEEDED(hr)) hr = actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        if (SUCCEEDED(hr)) hr = actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        if (SUCCEEDED(hr)) hr = actual->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &alignment);
        if (FAILED(hr) || channels != 2 || rate != kOutputRate || bits != 16 || alignment != 4) {
            error = HResultText(L"Windows could not convert this sound to stereo PCM. Try a PCM WAV file",
                                FAILED(hr) ? hr : E_FAIL); return {};
        }
        return stream;
    }

private:
    using Startup = HRESULT (WINAPI*)(ULONG, DWORD);
    using Shutdown = HRESULT (WINAPI*)();
    using CreateType = HRESULT (WINAPI*)(IMFMediaType**);
    using CreateReader = HRESULT (WINAPI*)(LPCWSTR, IMFAttributes*, IMFSourceReader**);
    HMODULE platformLibrary_ = nullptr;
    HMODULE readerLibrary_ = nullptr;
    Startup startup_ = nullptr;
    Shutdown shutdown_ = nullptr;
    CreateType createType_ = nullptr;
    CreateReader createReader_ = nullptr;
    bool started_ = false;
    bool comInitialized_ = false;
    bool attempted_ = false;
    std::wstring initializationError_;

    bool Initialize(std::wstring& error) {
        if (started_) return true;
        if (attempted_) { error = initializationError_; return false; }
        attempted_ = true;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInitialized_ = SUCCEEDED(com);
        platformLibrary_ = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        readerLibrary_ = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (platformLibrary_) {
            startup_ = Procedure<Startup>(platformLibrary_, "MFStartup");
            shutdown_ = Procedure<Shutdown>(platformLibrary_, "MFShutdown");
            createType_ = Procedure<CreateType>(platformLibrary_, "MFCreateMediaType");
        }
        if (readerLibrary_)
            createReader_ = Procedure<CreateReader>(readerLibrary_, "MFCreateSourceReaderFromURL");
        if (!comInitialized_ || !startup_ || !shutdown_ || !createType_ || !createReader_) {
            initializationError_ = L"Windows audio codec support is unavailable. Use a PCM WAV file, or install the Windows Media Feature Pack.";
        } else {
            const HRESULT hr = startup_(MF_VERSION, MFSTARTUP_FULL);
            started_ = SUCCEEDED(hr);
            if (!started_) initializationError_ = HResultText(L"Windows audio codecs could not start. Try PCM WAV", hr);
        }
        error = initializationError_;
        return started_;
    }
};

ClipPtr DecodeFile(const std::wstring& path, MediaDecoder& decoder,
                   const std::atomic<bool>& cancelled, std::wstring& error) {
    error.clear();
    if (path.empty()) return {};
    auto file = std::make_shared<FileHandle>();
    file->value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file->value == INVALID_HANDLE_VALUE) {
        error = L"The selected sound file could not be opened."; return {};
    }
    LARGE_INTEGER length{};
    if (GetFileType(file->value) != FILE_TYPE_DISK || !GetFileSizeEx(file->value, &length) || length.QuadPart <= 0) {
        error = L"Choose a regular sound file; devices, pipes and empty files are not supported."; return {};
    }
    ClipPtr parsed;
    bool fallback = false;
    if (DecodeWave(file, static_cast<std::uint64_t>(length.QuadPart), parsed, fallback, error) && !fallback) return parsed;
    if (cancelled.load()) return {};
    // The decoder receives the resolved local file path, never a URL or a
    // playlist chosen through the all-files picker. The open handle denies
    // writes/deletion for the lifetime of this configured source.
    if (!SupportedExtension(path)) {
        error = L"Choose WAV, MP3, WMA, FLAC, M4A or AAC audio."; return {};
    }
    std::array<wchar_t, 32768> resolved{};
    const DWORD count = GetFinalPathNameByHandleW(file->value, resolved.data(),
                                                static_cast<DWORD>(resolved.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= resolved.size()) { error = L"The sound file's local path could not be resolved."; return {}; }
    auto clip = std::make_shared<Clip>();
    clip->media = true;
    clip->file = file;
    clip->path.assign(resolved.data(), count);
    auto stream = decoder.Open(clip->path, error);
    if (!stream) return {};
    clip->samples.resize(kInitialFrames * 2);
    const auto frames = stream->Read(clip->samples.data(), kInitialFrames, cancelled, error);
    if (!error.empty() || cancelled.load()) return {};
    if (!frames) { error = L"The selected sound contains no audio samples."; return {}; }
    clip->samples.resize(frames * 2);
    clip->complete = stream->Finished();
    clip->outputFrames = clip->complete ? frames : 0;
    return clip;
}

struct CachedClip {
    std::wstring path;
    std::wstring error;
    ClipPtr original;
    FILETIME modified{};
    DWORD sizeHigh = 0;
    DWORD sizeLow = 0;
};

// ---- Output devices -------------------------------------------------------

// A device only moves already-mixed frames; the worker owns all decoding.
class OutputDevice {
public:
    virtual ~OutputDevice() = default;
    virtual bool Open(std::wstring& error) = 0;
    virtual void Close() noexcept = 0;
    virtual bool Start(std::wstring& error) = 0;
    virtual void Stop() noexcept = 0;
    virtual HANDLE Event() const noexcept = 0;
    virtual std::size_t WritableFrames() = 0;
    virtual bool Write(const std::int16_t* interleaved, std::size_t frames, std::wstring& error) = 0;
    virtual bool Invalidated() const noexcept = 0;
    virtual const wchar_t* Name() const noexcept = 0;
};

constexpr GUID kClsidDeviceEnumerator{0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr GUID kIidDeviceEnumerator{0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr GUID kIidAudioClient{0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
constexpr GUID kIidAudioRenderClient{0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
constexpr HRESULT kDeviceInvalidated = static_cast<HRESULT>(0x88890004); // AUDCLNT_E_DEVICE_INVALIDATED

class WasapiDevice final : public OutputDevice {
public:
    ~WasapiDevice() override { Close(); if (event_) CloseHandle(event_); }
    bool Open(std::wstring& error) override {
        Close();
        invalidated_ = false;
        if (!event_) event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) { error = L"Could not create the audio output event."; return false; }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(kClsidDeviceEnumerator, nullptr, CLSCTX_ALL, kIidDeviceEnumerator, enumerator.PutVoid());
        if (FAILED(hr)) { error = HResultText(L"The Windows audio service is unavailable", hr); return false; }
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.Put());
        if (FAILED(hr)) {
            error = hr == E_NOTFOUND ? L"No audio output device is available." : HResultText(L"Could not find the default audio output", hr);
            return false;
        }
        hr = device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, client_.PutVoid());
        if (FAILED(hr)) { error = HResultText(L"Could not open the audio output", hr); return false; }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2; format.nSamplesPerSec = kOutputRate;
        format.wBitsPerSample = 16; format.nBlockAlign = 4;
        format.nAvgBytesPerSec = kOutputRate * 4;
        // Shared mode with the engine's own converter: the device keeps its
        // configured format and this stream always mixes 48 kHz stereo PCM.
        constexpr REFERENCE_TIME bufferDuration = 300000; // 30 ms of slack above the 10 ms period.
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            bufferDuration, 0, &format, nullptr);
        if (FAILED(hr)) {
            error = hr == AUDCLNT_E_DEVICE_IN_USE ? L"Another application is using the audio device exclusively."
                                                   : HResultText(L"Could not initialize the audio output", hr);
            Close(); return false;
        }
        if (FAILED(hr = client_->SetEventHandle(event_)) || FAILED(hr = client_->GetBufferSize(&bufferFrames_)) ||
            FAILED(hr = client_->GetService(kIidAudioRenderClient, render_.PutVoid()))) {
            error = HResultText(L"Could not prepare the audio output buffers", hr);
            Close(); return false;
        }
        return true;
    }
    void Close() noexcept override {
        if (client_ && started_) client_->Stop();
        started_ = false;
        render_.Reset();
        client_.Reset();
        bufferFrames_ = 0;
    }
    bool Start(std::wstring& error) override {
        if (!client_) { error = L"The audio output is not open."; return false; }
        if (started_) return true;
        client_->Reset();
        const HRESULT hr = client_->Start();
        if (FAILED(hr)) {
            if (hr == kDeviceInvalidated) invalidated_ = true;
            error = HResultText(L"Could not start the audio output", hr); return false;
        }
        started_ = true;
        return true;
    }
    void Stop() noexcept override {
        if (client_ && started_) { client_->Stop(); client_->Reset(); }
        started_ = false;
    }
    HANDLE Event() const noexcept override { return event_; }
    std::size_t WritableFrames() override {
        if (!client_) return 0;
        UINT32 padding = 0;
        const HRESULT hr = client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) { if (hr == kDeviceInvalidated) invalidated_ = true; return 0; }
        return padding >= bufferFrames_ ? 0u : bufferFrames_ - padding;
    }
    bool Write(const std::int16_t* interleaved, std::size_t frames, std::wstring& error) override {
        BYTE* data = nullptr;
        HRESULT hr = render_->GetBuffer(static_cast<UINT32>(frames), &data);
        if (FAILED(hr) || !data) {
            if (hr == kDeviceInvalidated) invalidated_ = true;
            error = HResultText(L"The audio output rejected a buffer", hr); return false;
        }
        std::memcpy(data, interleaved, frames * 4);
        hr = render_->ReleaseBuffer(static_cast<UINT32>(frames), 0);
        if (FAILED(hr)) { if (hr == kDeviceInvalidated) invalidated_ = true; error = HResultText(L"The audio output could not accept a buffer", hr); return false; }
        return true;
    }
    bool Invalidated() const noexcept override { return invalidated_; }
    const wchar_t* Name() const noexcept override { return L"WASAPI shared"; }
private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE event_ = nullptr;
    UINT32 bufferFrames_ = 0;
    bool started_ = false, invalidated_ = false;
};

// Legacy fallback with four 20 ms buffers. Headers stay prepared for the
// lifetime of the open device and are only released after waveOutReset.
class WaveOutDevice final : public OutputDevice {
public:
    static constexpr std::size_t kBufferFrames = kOutputRate / 50;
    static constexpr unsigned kBuffers = 4;
    ~WaveOutDevice() override { Close(); if (event_) CloseHandle(event_); }
    bool Open(std::wstring& error) override {
        Close();
        if (!event_) event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) { error = L"Could not create the audio output event."; return false; }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2; format.nSamplesPerSec = kOutputRate;
        format.wBitsPerSample = 16; format.nBlockAlign = 4;
        format.nAvgBytesPerSec = kOutputRate * 4;
        const MMRESULT result = waveOutOpen(&device_, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event_), 0, CALLBACK_EVENT);
        if (result != MMSYSERR_NOERROR) { device_ = nullptr; error = WaveError(result); return false; }
        for (auto& buffer : buffers_) {
            buffer.header = {};
            buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
            buffer.header.dwBufferLength = static_cast<DWORD>(buffer.samples.size() * sizeof(std::int16_t));
            if (waveOutPrepareHeader(device_, &buffer.header, sizeof(buffer.header)) == MMSYSERR_NOERROR) {
                buffer.prepared = true;
                buffer.header.dwFlags |= WHDR_DONE; // Free until the first write.
            }
        }
        return true;
    }
    void Close() noexcept override {
        if (!device_) return;
        waveOutReset(device_);
        bool retained = false;
        for (auto& buffer : buffers_) {
            if (buffer.prepared && waveOutUnprepareHeader(device_, &buffer.header, sizeof(buffer.header)) == MMSYSERR_NOERROR)
                buffer.prepared = false;
            retained |= buffer.prepared;
        }
        // A driver that refuses to release its headers keeps the handle alive
        // until process exit instead of risking a use-after-free.
        if (!retained && waveOutClose(device_) == MMSYSERR_NOERROR) device_ = nullptr;
        else if (retained) device_ = nullptr;
    }
    bool Start(std::wstring&) override { return device_ != nullptr; }
    void Stop() noexcept override { if (device_) waveOutReset(device_); }
    HANDLE Event() const noexcept override { return event_; }
    std::size_t WritableFrames() override {
        std::size_t frames = 0;
        for (const auto& buffer : buffers_)
            if (buffer.prepared && (buffer.header.dwFlags & WHDR_DONE)) frames += kBufferFrames;
        return frames;
    }
    bool Write(const std::int16_t* interleaved, std::size_t frames, std::wstring& error) override {
        std::size_t written = 0;
        while (written < frames) {
            Buffer* free = nullptr;
            for (auto& buffer : buffers_) if (buffer.prepared && (buffer.header.dwFlags & WHDR_DONE)) { free = &buffer; break; }
            if (!free) return true; // The caller only writes what WritableFrames allowed.
            const std::size_t count = std::min(kBufferFrames, frames - written);
            std::memcpy(free->samples.data(), interleaved + written * 2, count * 4);
            free->header.dwBufferLength = static_cast<DWORD>(count * 4);
            free->header.dwFlags &= ~WHDR_DONE;
            const MMRESULT result = waveOutWrite(device_, &free->header, sizeof(free->header));
            if (result != MMSYSERR_NOERROR) { free->header.dwFlags |= WHDR_DONE; error = WaveError(result); return false; }
            written += count;
        }
        return true;
    }
    bool Invalidated() const noexcept override { return false; }
    const wchar_t* Name() const noexcept override { return L"waveOut"; }
private:
    struct Buffer { WAVEHDR header{}; std::array<std::int16_t, kBufferFrames * 2> samples{}; bool prepared = false; };
    HWAVEOUT device_ = nullptr;
    std::array<Buffer, kBuffers> buffers_;
    HANDLE event_ = nullptr;
};

struct Voice {
    ClipPtr clip;
    Group group = Group::Mouse;
    SourceWindow source;
    std::unique_ptr<MediaStream> stream;
    std::uint64_t position = 0;  // Next source frame to decode into the ring.
    bool ended = false;          // Source exhausted; ring may still hold audio.
    std::array<std::int16_t, kPrefetchFrames * 2> ring{};
    std::size_t ringRead = 0, ringCount = 0;
    bool Active() const noexcept { return clip != nullptr; }
    void Reset() noexcept { clip.reset(); stream.reset(); source.count = 0; position = 0; ended = false; ringRead = ringCount = 0; }
};
} // namespace

namespace audio_detail {
// Developer checks replace the real device with a deterministic sink.
std::function<std::unique_ptr<OutputDevice>()> deviceFactoryOverride;
}

struct ClickAudio::Impl {
    std::thread worker;
    std::atomic<HANDLE> wake{nullptr};
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> allowed{true};
    std::atomic<bool> mouseEnabled{false};
    std::atomic<bool> keyboardEnabled{false};
    std::atomic<bool> loading{true};
    std::atomic<bool> settingsDirty{false};
    std::array<std::atomic<std::uint8_t>, kRequestCount> pending{};
    std::atomic<HWND> notifyWindow{nullptr};
    std::atomic<UINT> notifyMessage{0};
    std::atomic<unsigned> mouseVolume{75}, keyboardVolume{60};
    std::mutex settingsMutex;
    AudioOptions pendingOptions;
    mutable std::mutex errorMutex;
    std::wstring error;
    std::wstring outputName;
    // Worker-owned state below.
    AudioOptions options;
    std::array<CachedClip, kSlotCount> cache;
    std::array<ClipPtr, kSlotCount> playback;
    unsigned keyVariantCount = 0;
    unsigned lastKeyVariant = 0;
    std::uint32_t randomState = 0x9e3779b9u;
    std::wstring keyboardError, outputError;
    std::array<std::unique_ptr<Voice>, kMaximumVoices> voices;
    std::unique_ptr<OutputDevice> device;
    bool deviceOpen = false, deviceStarted = false;
    ULONGLONG deviceIdleSince = 0, deviceRetryAt = 0;
    std::array<std::int16_t, kMixFrames * 2> mixBuffer{};

    Impl() { for (auto& voice : voices) voice = std::make_unique<Voice>(); }
    ~Impl() { if (HANDLE event = wake.load()) CloseHandle(event); }
    void Signal() noexcept { if (HANDLE event = wake.load()) SetEvent(event); }
    void Notify() noexcept {
        const HWND window = notifyWindow.load();
        const UINT message = notifyMessage.load();
        if (window && message) PostMessageW(window, message, 0, 0);
    }
    void PublishError() {
        std::wstring value;
        const auto append = [&](const wchar_t* prefix, const std::wstring& text) {
            if (text.empty()) return;
            if (!value.empty()) value += L" ";
            value += prefix; value += text;
        };
        // A file problem only matters while its sounds are switched on;
        // turning them on reloads the files and reports it then.
        if (options.mouseEnabled) {
            append(L"Left sound: ", cache[kSlotLeft].error);
            append(L"Right sound: ", cache[kSlotRight].error);
            append(L"Middle sound: ", cache[kSlotMiddle].error);
        }
        if (options.keyboardEnabled) append(L"Keyboard sounds: ", keyboardError);
        append(L"", outputError);
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            error = std::move(value);
            outputName = deviceOpen && device ? device->Name() : L"";
        }
        Notify();
    }

    void Enqueue(Request request) noexcept {
        auto& counter = pending[request];
        std::uint8_t current = counter.load(std::memory_order_relaxed);
        while (current < kMaximumQueuedPerRequest &&
               !counter.compare_exchange_weak(current, static_cast<std::uint8_t>(current + 1),
                                              std::memory_order_release, std::memory_order_relaxed)) {}
        Signal();
    }

    // ---- Source loading (worker) ----
    void Load(CachedClip& entry, const std::wstring& path, MediaDecoder& decoder) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        const bool exists = !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes);
        if (entry.path == path && (path.empty() || (entry.original && exists &&
            CompareFileTime(&entry.modified, &attributes.ftLastWriteTime) == 0 &&
            entry.sizeHigh == attributes.nFileSizeHigh && entry.sizeLow == attributes.nFileSizeLow))) return;
        entry.path = path;
        entry.original.reset();
        entry.error.clear();
        entry.modified = attributes.ftLastWriteTime;
        entry.sizeHigh = attributes.nFileSizeHigh;
        entry.sizeLow = attributes.nFileSizeLow;
        if (!path.empty()) entry.original = DecodeFile(path, decoder, stopping, entry.error);
    }
    static bool IsDirectory(const std::wstring& path) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    // A keyboard set folder holds key*.wav (up to six variants), space.*,
    // enter.* and backspace.*. Missing special keys fall back to key variants.
    void LoadKeyboardSet(MediaDecoder& decoder) {
        std::array<std::wstring, kSlotCount> paths{};
        keyboardError.clear();
        const auto& root = options.keyboard;
        if (!root.empty()) {
            if (IsDirectory(root)) {
                std::vector<std::wstring> keys;
                WIN32_FIND_DATAW item{};
                const HANDLE search = FindFirstFileW((root + L"\\*").c_str(), &item);
                if (search != INVALID_HANDLE_VALUE) {
                    unsigned inspected = 0;
                    do {
                        if (item.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                        const std::wstring name = item.cFileName;
                        if (!SupportedExtension(name)) continue;
                        std::wstring stem = name.substr(0, name.find_last_of(L'.'));
                        std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                        const std::wstring full = root + L"\\" + name;
                        if (stem == L"space") paths[kSlotSpace] = full;
                        else if (stem == L"enter" || stem == L"return") paths[kSlotEnter] = full;
                        else if (stem == L"backspace") paths[kSlotBackspace] = full;
                        else if (stem.rfind(L"key", 0) == 0) keys.push_back(full);
                    } while (++inspected < 512 && FindNextFileW(search, &item));
                    FindClose(search);
                }
                std::sort(keys.begin(), keys.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
                for (unsigned n = 0; n < kKeyVariants && n < keys.size(); ++n) paths[kSlotKeyFirst + n] = keys[n];
                if (keys.empty()) keyboardError = L"The keyboard sound folder has no key*.wav file. Choose a set from sounds\\keyboard or a single sound file.";
            } else {
                paths[kSlotKeyFirst] = root;
            }
        }
        keyVariantCount = 0;
        for (unsigned slot = kSlotKeyFirst; slot < kSlotCount; ++slot) {
            Load(cache[slot], paths[slot], decoder);
            if (stopping.load()) return;
            if (!cache[slot].error.empty() && keyboardError.empty()) keyboardError = cache[slot].error;
            playback[slot] = cache[slot].original;
            if (slot <= kSlotKeyLast && playback[slot]) keyVariantCount = slot - kSlotKeyFirst + 1;
        }
        // Keep variants contiguous even if a middle file failed to decode.
        unsigned write = kSlotKeyFirst;
        for (unsigned slot = kSlotKeyFirst; slot <= kSlotKeyLast; ++slot)
            if (playback[slot]) playback[write++] = playback[slot];
        keyVariantCount = write - kSlotKeyFirst;
        for (unsigned slot = write; slot <= kSlotKeyLast; ++slot) playback[slot].reset();
    }
    void ReadOptions(MediaDecoder& decoder) {
        AudioOptions next;
        { std::lock_guard<std::mutex> lock(settingsMutex); next = pendingOptions; }
        if ((options.mouseEnabled && !next.mouseEnabled) || next.mouseVolume == 0) StopGroup(Group::Mouse);
        if ((options.keyboardEnabled && !next.keyboardEnabled) || next.keyboardVolume == 0) StopGroup(Group::Keyboard);
        options = std::move(next);
        Load(cache[kSlotLeft], options.left, decoder);
        if (options.right == options.left) cache[kSlotRight] = cache[kSlotLeft];
        else Load(cache[kSlotRight], options.right, decoder);
        if (options.middle == options.left) cache[kSlotMiddle] = cache[kSlotLeft];
        else if (options.middle == options.right) cache[kSlotMiddle] = cache[kSlotRight];
        else Load(cache[kSlotMiddle], options.middle, decoder);
        for (unsigned slot = kSlotLeft; slot <= kSlotMiddle; ++slot) playback[slot] = cache[slot].original;
        if (stopping.load()) return;
        LoadKeyboardSet(decoder);
        if (stopping.load()) return;
        if (!settingsDirty.load()) loading.store(false);
        PublishError();
    }

    // ---- Device lifetime (worker) ----
    bool EnsureDevice() {
        if (deviceOpen && device && !device->Invalidated()) return true;
        const ULONGLONG now = GetTickCount64();
        if (deviceOpen) { CloseDevice(); }
        if (now < deviceRetryAt) return false;
        if (!device) device = audio_detail::deviceFactoryOverride ? audio_detail::deviceFactoryOverride() : std::make_unique<WasapiDevice>();
        std::wstring failure;
        if (!device->Open(failure)) {
            // The legacy path covers unusual drivers and virtual desktops.
            if (!audio_detail::deviceFactoryOverride && dynamic_cast<WasapiDevice*>(device.get())) {
                device = std::make_unique<WaveOutDevice>();
                std::wstring legacyFailure;
                if (device->Open(legacyFailure)) { deviceOpen = true; outputError.clear(); PublishError(); return true; }
            }
            outputError = failure;
            deviceRetryAt = now + kDeviceRetryMs;
            device.reset(); // The next attempt starts again with the preferred path.
            PublishError();
            return false;
        }
        deviceOpen = true;
        if (!outputError.empty()) { outputError.clear(); PublishError(); } else PublishError();
        return true;
    }
    void CloseDevice() noexcept {
        if (device) device->Close();
        device.reset();
        deviceOpen = deviceStarted = false;
    }

    // ---- Voices (worker) ----
    bool AnyVoice() const noexcept {
        for (const auto& voice : voices) if (voice->Active()) return true;
        return false;
    }
    void StopGroup(Group group) noexcept {
        for (auto& voice : voices) if (voice->Active() && voice->group == group) voice->Reset();
    }
    void StopVoices() noexcept { for (auto& voice : voices) voice->Reset(); }
    void StartVoice(const ClipPtr& clip, Group group, MediaDecoder& decoder) {
        if (!clip || clip->samples.empty()) return;
        Voice* selected = nullptr;
        for (auto& voice : voices) if (!voice->Active()) { selected = voice.get(); break; }
        if (!selected) {
            // Bounded overlap: replace the voice closest to its end rather
            // than queueing an unbounded backlog.
            for (auto& voice : voices)
                if (!selected || voice->position > selected->position) selected = voice.get();
        }
        selected->Reset();
        selected->clip = clip;
        selected->group = group;
        TopUp(*selected, decoder);
    }
    // Decodes up to half a ring per call so one slow codec read cannot stall
    // the device for long; the device buffer and ring absorb the rest.
    void TopUp(Voice& voice, MediaDecoder& decoder) {
        if (!voice.Active() || voice.ended) return;
        const auto& clip = *voice.clip;
        std::size_t produced = 0;
        std::wstring readError;
        while (voice.ringCount < kPrefetchFrames && produced < kPrefetchFrames / 2 && !stopping.load()) {
            const std::size_t at = ((voice.ringRead + voice.ringCount) % kPrefetchFrames) * 2;
            if (voice.position < clip.samples.size() / 2) {
                const auto source = static_cast<std::size_t>(voice.position) * 2;
                voice.ring[at] = clip.samples[source]; voice.ring[at + 1] = clip.samples[source + 1];
                ++voice.position; ++voice.ringCount; ++produced;
            } else if (!clip.media) {
                if (voice.position >= clip.outputFrames || !OutputFrame(clip, voice.source, voice.position, voice.ring[at], voice.ring[at + 1])) {
                    if (voice.position < clip.outputFrames) readError = L"The WAV file could not be read during playback.";
                    voice.ended = true; break;
                }
                ++voice.position; ++voice.ringCount; ++produced;
            } else if (clip.complete) { voice.ended = true; break; }
            else {
                if (!voice.stream) {
                    voice.stream = decoder.Open(clip.path, readError);
                    if (!voice.stream) { voice.ended = true; break; }
                    // Exact prefix consumption avoids approximate codec seeking.
                    std::array<std::int16_t, kInitialFrames * 2> discarded{};
                    const auto skip = clip.samples.size() / 2;
                    if (voice.stream->Read(discarded.data(), skip, stopping, readError) != skip) {
                        if (readError.empty() && !stopping.load()) readError = L"The sound changed before playback.";
                        voice.ended = true; break;
                    }
                }
                // Decode contiguous frames straight into the ring tail.
                const std::size_t tail = (voice.ringRead + voice.ringCount) % kPrefetchFrames;
                const std::size_t room = std::min({kPrefetchFrames - voice.ringCount, kPrefetchFrames - tail, kPrefetchFrames / 2 - produced});
                const auto received = voice.stream->Read(voice.ring.data() + tail * 2, room, stopping, readError);
                voice.ringCount += received; voice.position += received; produced += received;
                if (!received || voice.stream->Finished() || !readError.empty()) voice.ended = true;
                if (!received) break;
            }
        }
        if (!readError.empty()) { outputError = readError; PublishError(); }
    }
    // Mixes one chunk from the voice rings; returns the frames written.
    std::size_t Mix(std::int16_t* output, std::size_t frames) {
        std::array<float, kMixFrames * 2> accumulator{};
        bool active = false;
        const float mouseGain = mouseVolume.load(std::memory_order_relaxed) / 100.f;
        const float keyboardGain = keyboardVolume.load(std::memory_order_relaxed) / 100.f;
        for (auto& entry : voices) {
            auto& voice = *entry;
            if (!voice.Active()) continue;
            if (voice.ringCount == 0) {
                // A finished voice is released here. A live voice whose ring
                // is momentarily empty waits for its next TopUp instead of
                // having silence spliced into the stream.
                if (voice.ended) voice.Reset();
                continue;
            }
            const float gain = voice.group == Group::Mouse ? mouseGain : keyboardGain;
            const std::size_t count = std::min(frames, voice.ringCount);
            for (std::size_t frame = 0; frame < count; ++frame) {
                const std::size_t at = ((voice.ringRead + frame) % kPrefetchFrames) * 2;
                accumulator[frame * 2] += voice.ring[at] * gain;
                accumulator[frame * 2 + 1] += voice.ring[at + 1] * gain;
            }
            voice.ringRead = (voice.ringRead + count) % kPrefetchFrames;
            voice.ringCount -= count;
            active = true;
            if (voice.ended && voice.ringCount == 0) voice.Reset();
        }
        if (!active) return 0;
        for (std::size_t i = 0; i < frames * 2; ++i)
            output[i] = static_cast<std::int16_t>(std::clamp(std::lround(accumulator[i]), -32768l, 32767l));
        return frames;
    }
    void ProcessPending(MediaDecoder& decoder) {
        std::array<std::uint8_t, kRequestCount> counts{};
        bool any = false;
        for (unsigned i = 0; i < kRequestCount; ++i) { counts[i] = pending[i].exchange(0, std::memory_order_acquire); any |= counts[i] != 0; }
        if (!any) return;
        if (!allowed.load()) { StopVoices(); return; }
        const auto keyClip = [&]() -> ClipPtr {
            if (!keyVariantCount) return {};
            if (keyVariantCount == 1) return playback[kSlotKeyFirst];
            // Skip the previous variant so repeated keys never machine-gun one sample.
            randomState = randomState * 1664525u + 1013904223u;
            unsigned variant = (randomState >> 16) % (keyVariantCount - 1);
            if (variant >= lastKeyVariant) ++variant;
            lastKeyVariant = variant;
            return playback[kSlotKeyFirst + variant];
        };
        const auto categoryClip = [&](Slot special) { return playback[special] ? playback[special] : keyClip(); };
        unsigned maximum = 0;
        for (const auto count : counts) maximum = std::max<unsigned>(maximum, count);
        for (unsigned round = 0; round < maximum && allowed.load(); ++round) {
            const auto take = [&](Request request) { return counts[request] > round; };
            const bool mouse = mouseEnabled.load() && !loading.load();
            const bool keyboard = keyboardEnabled.load() && !loading.load();
            if (mouse && take(kRequestLeft)) StartVoice(playback[kSlotLeft], Group::Mouse, decoder);
            if (mouse && take(kRequestRight)) StartVoice(playback[kSlotRight], Group::Mouse, decoder);
            if (mouse && take(kRequestMiddle)) StartVoice(playback[kSlotMiddle], Group::Mouse, decoder);
            if (keyboard && take(kRequestKey)) StartVoice(keyClip(), Group::Keyboard, decoder);
            if (keyboard && take(kRequestSpace)) StartVoice(categoryClip(kSlotSpace), Group::Keyboard, decoder);
            if (keyboard && take(kRequestEnter)) StartVoice(categoryClip(kSlotEnter), Group::Keyboard, decoder);
            if (keyboard && take(kRequestBackspace)) StartVoice(categoryClip(kSlotBackspace), Group::Keyboard, decoder);
            if (take(kRequestPreviewLeft)) StartVoice(playback[kSlotLeft], Group::Mouse, decoder);
            if (take(kRequestPreviewRight)) StartVoice(playback[kSlotRight], Group::Mouse, decoder);
            if (take(kRequestPreviewMiddle)) StartVoice(playback[kSlotMiddle], Group::Mouse, decoder);
            if (take(kRequestPreviewKey)) StartVoice(keyClip(), Group::Keyboard, decoder);
        }
    }
    // Moves mixed audio into the device while it has room, then refills rings.
    void Service(MediaDecoder& decoder) {
        if (!AnyVoice()) {
            if (deviceStarted && GetTickCount64() - deviceIdleSince > 400) { device->Stop(); deviceStarted = false; }
            return;
        }
        if (!EnsureDevice()) { StopVoices(); return; }
        std::wstring failure;
        if (!deviceStarted) {
            if (!device->Start(failure)) {
                outputError = failure; deviceRetryAt = GetTickCount64() + kDeviceRetryMs;
                CloseDevice(); StopVoices(); PublishError(); return;
            }
            deviceStarted = true;
        }
        std::size_t writable = device->WritableFrames();
        unsigned chunks = 0;
        while (writable >= kMixFrames && chunks++ < 16) {
            // Refill short rings before mixing so a chunk never pads a live
            // voice with silence while the device still has room.
            for (auto& voice : voices)
                if (voice->Active() && !voice->ended && voice->ringCount < kMixFrames) TopUp(*voice, decoder);
            const std::size_t frames = Mix(mixBuffer.data(), kMixFrames);
            if (!frames) break;
            if (!device->Write(mixBuffer.data(), frames, failure)) {
                outputError = failure; deviceRetryAt = GetTickCount64() + kDeviceRetryMs;
                CloseDevice(); StopVoices(); PublishError(); return;
            }
            writable -= frames;
        }
        for (auto& voice : voices) TopUp(*voice, decoder);
        deviceIdleSince = GetTickCount64();
    }
    void ThreadMain() noexcept {
        // The worker owns COM for WASAPI and Media Foundation alike. All
        // SourceReaders and the audio client are released before this scope ends.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            MediaDecoder decoder;
            try {
                while (!stopping.load()) {
                    if (!allowed.load()) StopVoices();
                    if (settingsDirty.exchange(false)) ReadOptions(decoder);
                    if (stopping.load()) break;
                    if (settingsDirty.load()) continue;
                    ProcessPending(decoder);
                    Service(decoder);
                    const bool active = AnyVoice();
                    if (!active && deviceOpen && !deviceStarted && GetTickCount64() - deviceIdleSince > kDeviceIdleCloseMs) {
                        CloseDevice();
                        PublishError();
                    }
                    HANDLE handles[2] = {wake.load(), deviceOpen && device ? device->Event() : nullptr};
                    const DWORD count = handles[1] ? 2u : 1u;
                    DWORD timeout = INFINITE;
                    if (active) timeout = 10;
                    else if (deviceOpen) timeout = deviceStarted ? 50 : static_cast<DWORD>(kDeviceIdleCloseMs);
                    WaitForMultipleObjects(count, handles, FALSE, timeout);
                }
            } catch (...) {
                try {
                    outputError = L"The sound worker stopped after an audio error. Restart ClickShade to retry.";
                    PublishError();
                } catch (...) {}
            }
            StopVoices();
            CloseDevice();
            playback = {};
            for (auto& entry : cache) entry = {};
        }
        if (SUCCEEDED(com)) CoUninitialize();
        running.store(false);
    }
};

ClickAudio::ClickAudio() : impl_(std::make_unique<Impl>()) {}
ClickAudio::~ClickAudio() { Stop(); }

bool ClickAudio::Start(const Settings& settings, std::wstring& error) {
    Stop();
    if (!impl_->wake.load()) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) { error = L"Could not create the sound worker's event."; return false; }
        impl_->wake.store(event);
    }
    impl_->stopping.store(false);
    for (auto& counter : impl_->pending) counter.store(0);
    impl_->outputError.clear();
    UpdateSettings(settings);
    impl_->running.store(true);
    try { impl_->worker = std::thread([this] { impl_->ThreadMain(); }); }
    catch (...) {
        impl_->running.store(false);
        error = L"Could not start the sound worker.";
        return false;
    }
    return true;
}

void ClickAudio::Stop() {
    impl_->running.store(false);
    impl_->stopping.store(true);
    for (auto& counter : impl_->pending) counter.store(0);
    impl_->Signal();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void ClickAudio::UpdateSettings(const Settings& settings) {
    AudioOptions options = OptionsFrom(settings);
    impl_->mouseEnabled.store(options.mouseEnabled);
    impl_->keyboardEnabled.store(options.keyboardEnabled);
    impl_->mouseVolume.store(static_cast<unsigned>(options.mouseVolume));
    impl_->keyboardVolume.store(static_cast<unsigned>(options.keyboardVolume));
    bool reload = true;
    {
        std::lock_guard<std::mutex> lock(impl_->settingsMutex);
        const auto& previous = impl_->pendingOptions;
        // Volume-only edits must not reopen sources or interrupt playback.
        reload = previous.left != options.left || previous.right != options.right ||
                 previous.middle != options.middle || previous.keyboard != options.keyboard ||
                 previous.mouseEnabled != options.mouseEnabled || previous.keyboardEnabled != options.keyboardEnabled ||
                 (previous.mouseVolume != 0) != (options.mouseVolume != 0) ||
                 (previous.keyboardVolume != 0) != (options.keyboardVolume != 0) || !impl_->running.load();
        impl_->pendingOptions = std::move(options);
    }
    if (reload) {
        impl_->loading.store(true);
        for (unsigned i = kRequestLeft; i <= kRequestBackspace; ++i) impl_->pending[i].store(0);
        impl_->settingsDirty.store(true);
    }
    impl_->Signal();
}

void ClickAudio::SetAllowed(bool allowed) noexcept {
    impl_->allowed.store(allowed);
    if (!allowed) for (auto& counter : impl_->pending) counter.store(0);
    impl_->Signal();
}

void ClickAudio::Click(unsigned mask) noexcept {
    if (!impl_->running.load() || !impl_->allowed.load() || !impl_->mouseEnabled.load() || impl_->loading.load()) return;
    if (mask & 1u) impl_->Enqueue(kRequestLeft);
    if (mask & 2u) impl_->Enqueue(kRequestRight);
    if (mask & 4u) impl_->Enqueue(kRequestMiddle);
}

void ClickAudio::Key(unsigned category) noexcept {
    if (!impl_->running.load() || !impl_->allowed.load() || !impl_->keyboardEnabled.load() || impl_->loading.load()) return;
    switch (category) {
    case kKeySpace: impl_->Enqueue(kRequestSpace); break;
    case kKeyEnter: impl_->Enqueue(kRequestEnter); break;
    case kKeyBackspace: impl_->Enqueue(kRequestBackspace); break;
    default: impl_->Enqueue(kRequestKey); break;
    }
}

void ClickAudio::Preview(unsigned mask) noexcept {
    if (!impl_->running.load() || !impl_->allowed.load()) return;
    if (mask & 1u) impl_->Enqueue(kRequestPreviewLeft);
    if (mask & 2u) impl_->Enqueue(kRequestPreviewRight);
    if (mask & 4u) impl_->Enqueue(kRequestPreviewMiddle);
}

void ClickAudio::PreviewKey() noexcept {
    if (!impl_->running.load() || !impl_->allowed.load()) return;
    impl_->Enqueue(kRequestPreviewKey);
}

std::wstring ClickAudio::LastError() const {
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    return impl_->error;
}

bool ClickAudio::Ready() const noexcept {
    return impl_->running.load() && !impl_->loading.load();
}

std::wstring ClickAudio::OutputDescription() const {
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    return impl_->outputName;
}

void ClickAudio::SetNotifyWindow(HWND target, UINT message) noexcept {
    impl_->notifyMessage.store(message);
    impl_->notifyWindow.store(target);
}

} // namespace clickshade
