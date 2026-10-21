// Developer-only, headless WAV/file-boundary regression checks. Including the
// implementation keeps parser test access out of the production audio API.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
#include "../audio.cpp"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <signal.h>
#include <sys/wait.h>

using namespace clickshade::linuxapp;
namespace {
unsigned passed = 0, failed = 0;
void Check(bool value, const char* name) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    if (value) ++passed; else ++failed;
}
void Put16(std::vector<unsigned char>& bytes, std::size_t at, unsigned value) {
    bytes[at] = static_cast<unsigned char>(value);
    bytes[at + 1] = static_cast<unsigned char>(value >> 8);
}
void Put32(std::vector<unsigned char>& bytes, std::size_t at, std::uint32_t value) {
    Put16(bytes, at, value); Put16(bytes, at + 2, value >> 16);
}
std::vector<unsigned char> Wave() {
    std::vector<unsigned char> bytes(52);
    std::memcpy(bytes.data(), "RIFF", 4); Put32(bytes, 4, 44);
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8); Put32(bytes, 16, 16);
    Put16(bytes, 20, 1); Put16(bytes, 22, 1); Put32(bytes, 24, 48000);
    Put32(bytes, 28, 96000); Put16(bytes, 32, 2); Put16(bytes, 34, 16);
    std::memcpy(bytes.data() + 36, "data", 4); Put32(bytes, 40, 8);
    Put16(bytes, 44, 0); Put16(bytes, 46, 32767);
    Put16(bytes, 48, 32768); Put16(bytes, 50, 16384);
    return bytes;
}
bool Write(const std::string& path, const std::vector<unsigned char>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return bool(file);
}
bool Rejected(const std::string& path, const std::vector<unsigned char>& bytes) {
    std::string error;
    return Write(path, bytes) && !LoadWave(path, 100, error) && !error.empty();
}
// A buggy FIFO open would hang a joining test thread. A disposable child gives
// the regression a hard deadline without leaving blocked workers behind.
bool Finishes(const std::function<bool()>& action, unsigned milliseconds = 1500) {
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child < 0) return false;
    if (!child) {
        bool okay = false;
        try { okay = action(); } catch (...) {}
        _exit(okay ? 0 : 1);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    int status = 0;
    do {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return false;
}
struct TemporaryDirectory {
    char path[64] = "/tmp/clickshade-audio-test-XXXXXX";
    bool created = mkdtemp(path) != nullptr;
    ~TemporaryDirectory() {
        if (created) { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    }
};
} // namespace

int main() {
    TemporaryDirectory temporary;
    if (!temporary.created) { std::puts("Could not create the isolated test directory."); return 2; }
    const std::string directory = temporary.path;
    const std::string regular = directory + "/regular.wav";
    const std::string malformed = directory + "/malformed.wav";
    const std::string fifo = directory + "/pipe.wav";
    std::string error;

    Check(!LoadWave("", 100, error) && error.empty(), "an unset sound path is silent and has no error");
    Check(Write(regular, Wave()), "regular PCM WAV fixture is written");
    const auto clip = LoadWave(regular, 100, error);
    Check(clip && error.empty() && clip->samples.size() == 8,
        "regular mono WAV decodes to the expected stereo sample count");
    Check(clip && clip->samples[0] == 0 && clip->samples[2] == 32766 &&
        clip->samples[4] == -32767 && clip->samples[6] == 16384 &&
        clip->samples[2] == clip->samples[3], "PCM signs, channel duplication and normalization are preserved");
    const auto silent = LoadWave(regular, 0, error);
    Check(silent && std::all_of(silent->samples.begin(), silent->samples.end(), [](auto sample) { return sample == 0; }),
        "zero volume produces a valid silent clip");
    Check(!LoadWave(directory + "/missing.wav", 100, error) && !error.empty(),
        "missing sound files fail clearly");
    Check(!LoadWave(directory, 100, error) && error.find("regular WAV") != std::string::npos,
        "directories are rejected before audio reads");
    Check(!LoadWave("/dev/null", 100, error) && error.find("regular WAV") != std::string::npos,
        "device paths are rejected before audio reads");
    const std::string link = directory + "/linked.wav";
    const bool linked = symlink(regular.c_str(), link.c_str()) == 0;
    Check(linked && bool(LoadWave(link, 100, error)), "a symlink to a regular WAV remains supported");

    const bool madeFifo = mkfifo(fifo.c_str(), 0600) == 0;
    Check(madeFifo && Finishes([&] {
        std::string message;
        return !LoadWave(fifo, 100, message) && message.find("regular WAV") != std::string::npos;
    }), "FIFO paths fail promptly without needing a writer");
    const std::string pipeLink = directory + "/linked-pipe.wav";
    const bool linkedPipe = madeFifo && symlink(fifo.c_str(), pipeLink.c_str()) == 0;
    Check(linkedPipe && Finishes([&] {
        std::string message;
        return !LoadWave(pipeLink, 100, message) && message.find("regular WAV") != std::string::npos;
    }), "a symlink cannot bypass the opened-file type check");
    Check(madeFifo && Finishes([&] {
        Settings settings; settings.leftSoundPath = fifo; settings.clickSoundsEnabled = true;
        LinuxAudio audio; std::string message;
        if (!audio.Start(settings, message)) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        bool rejected = false;
        do {
            rejected = audio.LastError().find("regular WAV") != std::string::npos;
            if (rejected) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        audio.Stop();
        return rejected;
    }), "the real audio worker reports a FIFO error and stops without hanging");
    Check(madeFifo && Finishes([&] {
        Settings settings; settings.middleSoundPath = fifo; settings.clickSoundsEnabled = true;
        LinuxAudio audio; std::string message;
        if (!audio.Start(settings, message)) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        bool rejected = false;
        do {
            const auto failure = audio.LastError();
            rejected = failure.find("Middle sound:") != std::string::npos && failure.find("regular WAV") != std::string::npos;
            if (rejected) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        audio.Stop();
        return rejected;
    }), "middle-button sound is decoded by its own slot and retains bounded file rejection");

    auto bytes = Wave(); bytes.resize(11);
    Check(Rejected(malformed, bytes), "truncated RIFF headers are rejected");
    bytes = Wave(); Put32(bytes, 4, 0xffffffffu);
    Check(Rejected(malformed, bytes), "RIFF length cannot exceed the bounded file");
    bytes = Wave(); Put32(bytes, 40, 0xffffffffu);
    Check(Rejected(malformed, bytes), "sample chunks cannot escape their RIFF container");
    bytes = Wave(); bytes.pop_back(); Put32(bytes, 4, 43); Put32(bytes, 40, 7);
    Check(Rejected(malformed, bytes), "incomplete PCM sample frames are rejected");
    bytes = Wave(); Put16(bytes, 22, 3); Put16(bytes, 32, 6);
    Check(Rejected(malformed, bytes), "unsupported channel counts are rejected");
    bytes = Wave(); Put32(bytes, 24, 0);
    Check(Rejected(malformed, bytes), "zero sample rate cannot reach resampling arithmetic");
    bytes = Wave(); Put16(bytes, 20, 0xfffe);
    Check(Rejected(malformed, bytes), "truncated extensible-format headers are rejected");
    const std::string large = directory + "/long.wav";
    constexpr std::uint32_t longFrames = 48000u * 1200u; // Twenty-minute sparse WAV.
    bytes = Wave(); bytes.resize(44);
    Put32(bytes, 4, 36 + longFrames * 2); Put32(bytes, 40, longFrames * 2);
    {
        std::ofstream file(large, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.seekp(44 + std::uint64_t(longFrames) * 2 - 1); file.put('\0');
        file.seekp(44 + std::uint64_t(48000) * 1100 * 2);
        const char signal[2] = {0, 64}; file.write(signal, sizeof(signal));
    }
    const auto longClip = LoadWave(large, 100, error);
    Check(longClip && error.empty() && longClip->outputFrames == longFrames,
        "valid long WAV has no ten-second or ten-MiB application limit");
    Check(longClip && longClip->samples.size() == kInitialFrames * 2 && sizeof(SourceWindow) <= 66 * 1024,
        "long-file initial cache and each streaming source window have fixed memory bounds");
    SourceWindow lateSource;
    std::int16_t left = 0, right = 0;
    Check(longClip && OutputFrame(*longClip, lateSource, std::uint64_t(48000) * 1100, left, right) && left == 16384 && right == left,
        "streaming reads actual late samples beyond the initial cached audio");
    SourceWindow overlappingSource;
    Check(longClip && OutputFrame(*longClip, overlappingSource, 50000, left, right) && left == 0 && right == 0 &&
        OutputFrame(*longClip, lateSource, std::uint64_t(48000) * 1100, left, right) && left == 16384,
        "overlapping voices retain independent source positions on the same opened file");
    std::filesystem::resize_file(large, 44 + kInitialFrames * 2);
    SourceWindow truncatedSource;
    Check(longClip && !OutputFrame(*longClip, truncatedSource, std::uint64_t(48000) * 600, left, right),
        "a file truncated during playback stops that stream safely");

    const std::string tonePath = directory + "/volume-tone.wav";
    bytes = Wave(); bytes.resize(44 + 24000 * 2);
    Put32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8)); Put32(bytes, 40, 24000 * 2);
    for (std::size_t at = 44; at < bytes.size(); at += 2) Put16(bytes, at, 16384);
    Write(tonePath, bytes);
    const auto tone = LoadWave(tonePath, 100, error);
    std::array<AudioVoice, kVoices> voices;
    voices[0].clip = tone;
    std::array<std::int16_t, 12000> mixed{};
    bool mixError = false;
    const auto report = [&](unsigned) { mixError = true; };
    Check(tone && MixFrames(voices, mixed.data(), 480, 100, report) == 480 && mixed[0] == 16384 && voices[0].at == 480,
          "production mixer starts an unscaled cached voice at the current full volume");
    Check(MixFrames(voices, mixed.data(), 480, 25, report) == 480 && mixed[0] == 4096 && voices[0].at == 960,
          "live volume changes the next buffer without restarting the active voice");
    Check(MixFrames(voices, mixed.data(), 5000, 50, report) == 5000 && mixed[0] == 8192 && mixed[9998] == 8192 &&
          voices[0].at == 5960 && voices[0].source.count && !mixError,
          "live gain stays consistent across the cached-prefix to streamed-sample boundary");
    Check(MixFrames(voices, mixed.data(), 480, 0, report) == 0 && !voices[0].clip,
          "muting stops and releases active voices instead of queuing silent long clips");
    const std::string volumePath = directory + "/worker-volume.wav";
    Write(volumePath, bytes);
    Check(Finishes([&] {
        Settings settings; settings.leftSoundPath = volumePath; settings.soundVolume = 75; settings.clickSoundsEnabled = true;
        LinuxAudio audio; std::string message;
        if (!audio.Start(settings, message)) return false;
        const auto deadline = Clock::now() + std::chrono::milliseconds(700);
        bool opened = false;
        do {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
                const auto target = std::filesystem::read_symlink(entry.path(), ec);
                if (!ec && target == volumePath) { opened = true; break; }
                ec.clear();
            }
            if (opened) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (Clock::now() < deadline);
        if (!opened) { audio.Stop(); return false; }
        unlink(volumePath.c_str());
        settings.soundVolume = 25; audio.Update(settings);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const bool okay = audio.LastError().find("Left sound:") == std::string::npos;
        audio.Stop(); return okay;
    }), "volume-only edits retain the opened sound and never reload a now-removed path");

    Check(madeFifo && Finishes([&] {
        Settings settings; settings.leftSoundPath = fifo; settings.clickSoundsEnabled = false;
        LinuxAudio audio; std::string message;
        if (!audio.Start(settings, message)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const bool quiet = audio.LastError().find("sound:") == std::string::npos;
        audio.Stop();
        return quiet;
    }), "file problems of switched-off click sounds are not reported as warnings");

    std::printf("\nLinux audio: %u passed, %u failed.\n", passed, failed);
    return failed ? 1 : 0;
}
