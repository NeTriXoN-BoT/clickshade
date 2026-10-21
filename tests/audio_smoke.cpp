// A focused decoder test includes the implementation in this test translation
// unit, so the production API need not expose its private PCM parser. Compile
// this file alone (do not also compile click_audio.cpp into this executable).
// These checks validate data, bounds and asynchronous diagnostics. They do not
// claim that an output device played an audible sound.
#include "../src/click_audio.cpp"
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
int passed = 0, failed = 0;
unsigned notifications = 0;
void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    condition ? ++passed : ++failed;
}
void Put16(std::vector<std::uint8_t>& out, unsigned value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void Put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
std::vector<std::uint8_t> Wave(unsigned tag, unsigned channels, unsigned rate,
                               unsigned bits, const std::vector<std::uint8_t>& samples) {
    std::vector<std::uint8_t> out{'R','I','F','F'};
    Put32(out, static_cast<std::uint32_t>(36 + samples.size() + (samples.size() & 1u)));
    out.insert(out.end(), {'W','A','V','E','f','m','t',' '});
    Put32(out, 16); Put16(out, tag); Put16(out, channels); Put32(out, rate);
    Put32(out, rate * channels * bits / 8); Put16(out, channels * bits / 8); Put16(out, bits);
    out.insert(out.end(), {'d','a','t','a'}); Put32(out, static_cast<std::uint32_t>(samples.size()));
    out.insert(out.end(), samples.begin(), samples.end());
    if (samples.size() & 1u) out.push_back(0);
    return out;
}
void PumpUntil(const std::function<bool()>& done) {
    const auto end = GetTickCount64() + 4000;
    while (!done() && GetTickCount64() < end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(5);
    }
}
LRESULT CALLBACK TestProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_APP + 99) { ++notifications; return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
struct TempFile {
    std::wstring path;
    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        wchar_t directory[MAX_PATH]{}, name[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"csa", 0, name))
            throw std::runtime_error("Could not prepare audio fixture");
        path = name;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool ok = file != INVALID_HANDLE_VALUE &&
            WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (!ok) { DeleteFileW(path.c_str()); throw std::runtime_error("Could not write audio fixture"); }
    }
    ~TempFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
};
std::vector<std::unique_ptr<TempFile>> fixtures;
clickshade::ClipPtr Decode(const std::vector<std::uint8_t>& wave, std::wstring& error) {
    auto file = std::make_unique<TempFile>(wave);
    const auto path = file->path;
    fixtures.push_back(std::move(file));
    clickshade::MediaDecoder decoder;
    std::atomic<bool> cancelled{false};
    return clickshade::DecodeFile(path, decoder, cancelled, error);
}
} // namespace

int wmain() {
    try {
        std::wstring error;
        const auto mono = Wave(WAVE_FORMAT_PCM, 1, 48000, 8, {0,128,255});
        auto clip = Decode(mono, error);
        Check(clip && error.empty() && clip->samples == std::vector<std::int16_t>{-32767,-32767,0,0,32511,32511},
              "8-bit mono PCM maps to centered stereo samples");
        const auto stereo = Wave(WAVE_FORMAT_PCM, 2, 48000, 16, {0,0x80,0xff,0x7f,0,0,0,0});
        clip = Decode(stereo, error);
        Check(clip && clip->samples == std::vector<std::int16_t>{-32767,32766,0,0},
              "16-bit stereo preserves channel separation and signed amplitude");
        const auto pcm24 = Wave(WAVE_FORMAT_PCM, 1, 48000, 24, {0,0,0x80,0xff,0xff,0x7f});
        clip = Decode(pcm24, error);
        Check(clip && clip->samples == std::vector<std::int16_t>{-32767,-32767,32767,32767},
              "24-bit signed PCM decodes both extrema");
        std::vector<std::uint8_t> floatBytes;
        const std::array<float, 5> values{-1.0f,0.0f,0.5f,1.0f,std::numeric_limits<float>::quiet_NaN()};
        for (float value : values) {
            std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); Put32(floatBytes, bits);
        }
        clip = Decode(Wave(WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 32, floatBytes), error);
        Check(clip && clip->samples == std::vector<std::int16_t>{-32767,-32767,0,0,16384,16384,32767,32767,0,0},
              "floating PCM clamps safely and silences non-finite samples");
        clip = Decode(Wave(WAVE_FORMAT_PCM, 1, 24000, 16, {0,0,0xff,0x7f}), error);
        Check(clip && clip->samples.size() == 8 && clip->samples[0] == 0 && clip->samples[2] == 16383 &&
              clip->samples[4] == 32766 && clip->samples[6] == 32766,
              "resampling preserves duration and interpolates between input samples");
        const auto original = Decode(pcm24, error);
        Check(original && original->samples.size() == 4 && original->complete,
              "short clips retain only their exact prepared samples");
        auto truncated = mono; truncated.pop_back();
        Check(!Decode(truncated, error) && !error.empty(), "truncated RIFF file is rejected");
        auto invalidChunk = mono; invalidChunk[40] = 0xff; invalidChunk[41] = 0xff;
        Check(!Decode(invalidChunk, error) && !error.empty(), "out-of-range data chunk is rejected");
        auto invalidFrame = stereo; invalidFrame[32] = 7;
        Check(!Decode(invalidFrame, error) && !error.empty(), "invalid PCM frame alignment is rejected");
        Check(!Decode(Wave(WAVE_FORMAT_PCM, 3, 48000, 16, {0,0,0,0,0,0}), error),
              "unsupported channel counts are rejected before conversion");
        auto longClip = Decode(Wave(WAVE_FORMAT_PCM, 1, 48000, 8, std::vector<std::uint8_t>(960001,128)), error);
        Check(longClip && longClip->outputFrames == 960001 && longClip->samples.size() == clickshade::kInitialFrames * 2,
              "twenty-second sound is accepted with a100ms bounded prefix");
        clickshade::SourceWindow windowed;
        std::int16_t laterLeft = 1, laterRight = 1;
        Check(longClip && clickshade::OutputFrame(*longClip, windowed, 900000, laterLeft, laterRight) &&
              laterLeft == 0 && laterRight == 0 && windowed.count <= clickshade::kSourceFrames,
              "late sound samples stream correctly through a bounded source window");
        Check(sizeof(windowed.bytes) == 65536, "per-voice source window is bounded at64KiB");
        auto sparseHeader = Wave(WAVE_FORMAT_PCM, 1, 48000, 16, {});
        constexpr std::uint32_t longFrames = 48000u * 3600u;
        const auto replace32 = [](std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
        };
        replace32(sparseHeader, 4, 36 + longFrames * 2);
        replace32(sparseHeader, 40, longFrames * 2);
        TempFile sparse(sparseHeader);
        HANDLE largeFile = CreateFileW(sparse.path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER end{}; end.QuadPart = 44 + static_cast<LONGLONG>(longFrames - 1) * 2;
        DWORD written = 0;
        const unsigned char marker[]{0xff,0x7f};
        const bool sparseWritten = largeFile != INVALID_HANDLE_VALUE && SetFilePointerEx(largeFile, end, nullptr, FILE_BEGIN) &&
            WriteFile(largeFile, marker, 2, &written, nullptr) && written == 2;
        if (largeFile != INVALID_HANDLE_VALUE) CloseHandle(largeFile);
        Check(sparseWritten, "one-hour sparse WAV fixture is prepared without duration-sized RAM");
        clickshade::MediaDecoder decoder;
        std::atomic<bool> cancelled{false};
        auto hour = clickshade::DecodeFile(sparse.path, decoder, cancelled, error);
        windowed.count = 0;
        Check(hour && hour->outputFrames == longFrames && hour->samples.size() == clickshade::kInitialFrames * 2 &&
              clickshade::OutputFrame(*hour, windowed, longFrames - 1, laterLeft, laterRight) && laterLeft == 32766 && laterRight == 32766,
              "one-hour clip streams final signed sample with constant prefix memory");
        HANDLE writer = CreateFileW(sparse.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(writer == INVALID_HANDLE_VALUE, "validated sound handle prevents concurrent replacement or truncation");
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        hour.reset();
        auto device = clickshade::DecodeFile(L"NUL", decoder, cancelled, error);
        Check(!device && !error.empty(), "device paths are rejected as sound files");


        TempFile validFile(mono), invalidFile(truncated);
        WNDCLASSW wc{}; wc.lpfnWndProc = TestProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ClickShade.AudioSmoke.Window";
        RegisterClassW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"Audio test", 0, 0,0,0,0,
                                    HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        clickshade::ClickAudio audio;
        clickshade::Settings settings;
        settings.leftSoundPath = validFile.path;
        settings.clickSoundsEnabled = true;
        audio.SetNotifyWindow(window, WM_APP + 99);
        const bool started = audio.Start(settings, error);
        Check(started, "sound worker starts independently of output-device availability");
        if (started) {
            PumpUntil([] { return notifications != 0; });
            Check(notifications != 0, "asynchronous file load notifies the UI");
            const auto outputStatus = audio.LastError();
            Check(outputStatus.empty() || outputStatus.find(L"Audio output:") != std::wstring::npos,
                  "valid on-disk WAV decodes before any output-device error");
            if (!outputStatus.empty()) std::wprintf(L"NOTE Playback device unavailable: %ls\n", outputStatus.c_str());
            notifications = 0;
            settings.leftSoundPath = invalidFile.path;
            audio.UpdateSettings(settings);
            PumpUntil([] { return notifications != 0; });
            Check(audio.LastError().find(L"WAV") != std::wstring::npos,
                  "invalid on-disk WAV reports an asynchronous decode error");
            notifications = 0;
            settings.leftSoundPath = validFile.path;
            settings.middleSoundPath = invalidFile.path;
            audio.UpdateSettings(settings);
            PumpUntil([] { return notifications != 0; });
            Check(audio.LastError().find(L"Middle sound:") != std::wstring::npos &&
                  audio.LastError().find(L"Left sound:") == std::wstring::npos,
                  "middle-button sound errors are isolated from valid left sound");
            notifications = 0;
            settings.clickSoundsEnabled = false;
            audio.UpdateSettings(settings);
            PumpUntil([] { return notifications != 0; });
            Check(audio.LastError().find(L"sound:") == std::wstring::npos,
                  "file problems of switched-off click sounds are not reported as warnings");
            audio.SetAllowed(false);
            audio.Click(7); audio.Preview(7);
            audio.Stop();
            Check(true, "disabled-session queues and worker stop complete safely");
        }
        if (window) DestroyWindow(window);
    } catch (const std::exception& exception) {
        std::printf("FAIL exception: %s\n", exception.what()); ++failed;
    }
    std::puts("NOTE Audible playback and compressed-file codecs require a Windows machine with an audio output device.");
    std::printf("Results: %d passed, %d failed.\n", passed, failed);
    return failed ? 1 : 0;
}
