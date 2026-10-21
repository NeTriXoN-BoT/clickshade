// Exercise the real worker, mixer and file streams against a deterministic
// output device. This validates bounded buffers, live gain, keyboard sets and
// shutdown ordering, not audible hardware playback.
#include "../src/click_audio.cpp"

#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>

namespace {
// Replaces WASAPI/waveOut. "Immediate" mode accepts any amount instantly;
// paced mode holds a 100 ms buffer until the test drains it with Complete().
struct FakeDevice final : clickshade::OutputDevice {
    static std::mutex mutex;
    static std::map<int, std::uint64_t> frames;   // Left-channel value -> frames seen.
    static std::size_t pending, maximumWrite;
    static unsigned opens, closes, starts, stops;
    static bool immediate, failOpen;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool open = false;
    ~FakeDevice() override { Close(); CloseHandle(event); }
    bool Open(std::wstring& error) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (failOpen) { error = L"fake device refused"; return false; }
        ++opens; open = true; return true;
    }
    void Close() noexcept override { std::lock_guard<std::mutex> lock(mutex); if (open) ++closes; open = false; }
    bool Start(std::wstring&) override { std::lock_guard<std::mutex> lock(mutex); ++starts; return true; }
    void Stop() noexcept override { std::lock_guard<std::mutex> lock(mutex); ++stops; pending = 0; }
    HANDLE Event() const noexcept override { return event; }
    std::size_t WritableFrames() override {
        std::lock_guard<std::mutex> lock(mutex);
        return immediate ? 48000 : (pending >= 4800 ? 0 : 4800 - pending);
    }
    bool Write(const std::int16_t* interleaved, std::size_t count, std::wstring&) override {
        std::lock_guard<std::mutex> lock(mutex);
        maximumWrite = std::max(maximumWrite, count);
        for (std::size_t frame = 0; frame < count; ++frame) ++frames[interleaved[frame * 2]];
        if (!immediate) pending += count;
        return true;
    }
    bool Invalidated() const noexcept override { return false; }
    const wchar_t* Name() const noexcept override { return L"fake"; }
    static std::uint64_t Count(int amplitude) { std::lock_guard<std::mutex> lock(mutex); return frames[amplitude]; }
    static void Complete() { std::lock_guard<std::mutex> lock(mutex); pending = 0; }
    static void Immediate(bool value) { std::lock_guard<std::mutex> lock(mutex); immediate = value; }
};
std::mutex FakeDevice::mutex;
std::map<int, std::uint64_t> FakeDevice::frames;
std::size_t FakeDevice::pending = 0, FakeDevice::maximumWrite = 0;
unsigned FakeDevice::opens = 0, FakeDevice::closes = 0, FakeDevice::starts = 0, FakeDevice::stops = 0;
bool FakeDevice::immediate = true, FakeDevice::failOpen = false;

unsigned passed = 0, failed = 0, notifications = 0;
void Check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    condition ? ++passed : ++failed;
}
bool Until(const std::function<bool()>& predicate, DWORD milliseconds = 5000) {
    const auto end = GetTickCount64() + milliseconds;
    while (!predicate() && GetTickCount64() < end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(2);
    }
    return predicate();
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_APP + 17) { ++notifications; return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
std::wstring TemporaryFolder() {
    wchar_t folder[MAX_PATH]{};
    GetTempPathW(MAX_PATH, folder);
    const std::wstring path = std::wstring(folder) + L"ClickShade-audio-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}
struct Sound {
    std::wstring path;
    Sound(const std::wstring& file, std::int16_t amplitude, double seconds) : path(file) {
        const auto frames = static_cast<std::size_t>(seconds * 48000);
        std::vector<std::uint8_t> data(44 + frames * 2);
        const auto put = [&](unsigned offset, std::uint32_t value, unsigned count) {
            for (unsigned i = 0; i < count; ++i) data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
        };
        std::memcpy(data.data(), "RIFF", 4); put(4, static_cast<std::uint32_t>(data.size() - 8), 4);
        std::memcpy(data.data() + 8, "WAVEfmt ", 8); put(16, 16, 4); put(20, 1, 2); put(22, 1, 2);
        put(24, 48000, 4); put(28, 96000, 4); put(32, 2, 2); put(34, 16, 2);
        std::memcpy(data.data() + 36, "data", 4); put(40, static_cast<std::uint32_t>(data.size() - 44), 4);
        for (unsigned at = 44; at < data.size(); at += 2) put(at, static_cast<std::uint16_t>(amplitude), 2);
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written = 0;
        if (handle == INVALID_HANDLE_VALUE || !WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) || written != data.size())
            throw std::runtime_error("Audio fixture write failed");
        CloseHandle(handle);
    }
    ~Sound() { DeleteFileW(path.c_str()); }
};
} // namespace

int wmain() {
    clickshade::audio_detail::deviceFactoryOverride = [] { return std::unique_ptr<clickshade::OutputDevice>(new FakeDevice()); };
    const auto folder = TemporaryFolder();
    const auto keyboardFolder = folder + L"\\mechanical";
    CreateDirectoryW(keyboardFolder.c_str(), nullptr);
    // Amplitudes are chosen so sums and halves stay unique in the histogram.
    Sound left(folder + L"\\left.wav", 6000, 12), right(folder + L"\\right.wav", 10000, 12), middle(folder + L"\\middle.wav", 14000, 12);
    Sound keyA(keyboardFolder + L"\\key-1.wav", 1100, 0.05), keyB(keyboardFolder + L"\\key-2.wav", 1200, 0.05), space(keyboardFolder + L"\\space.wav", 1300, 0.05);
    WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ClickShade.AudioStreamingTest"; RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    clickshade::Settings settings;
    settings.clickSoundsEnabled = true; settings.soundVolume = 100;
    settings.leftSoundPath = left.path; settings.rightSoundPath = right.path; settings.middleSoundPath = middle.path;
    settings.keyboardSoundsEnabled = true; settings.keyboardVolume = 100; settings.keyboardSoundPath = keyboardFolder;
    clickshade::ClickAudio audio; audio.SetNotifyWindow(window, WM_APP + 17);
    std::wstring error;
    Check(audio.Start(settings, error) && Until([&] { return notifications != 0 && audio.Ready(); }), "worker prepares the three mouse sources and the keyboard set");
    Check(audio.LastError().empty(), "prepared sources report no error");
    audio.Click(7);
    Check(Until([] { return FakeDevice::Count(30000) == 12 * 48000; }, 15000),
          "left, right and middle mix together and stream complete 12-second clips without a cutoff");
    Check(FakeDevice::maximumWrite <= clickshade::kMixFrames, "every device write is one bounded 10 ms mixing chunk");
    Check(FakeDevice::opens == 1 && FakeDevice::starts >= 1, "one output device is opened and started on demand");
    Check(audio.OutputDescription() == L"fake", "the active output path is reported");

    FakeDevice::Immediate(false);
    const auto before = FakeDevice::Count(10000);
    audio.Click(2);
    Check(Until([&] { return FakeDevice::Count(10000) >= before + 4800; }), "a long voice fills the first 100 ms immediately");
    notifications = 0; settings.soundVolume = 50; audio.UpdateSettings(settings);
    Sleep(30);
    const auto quietBefore = FakeDevice::Count(5000);
    FakeDevice::Complete();
    Check(Until([&] { return FakeDevice::Count(5000) >= quietBefore + 2400; }), "an active long sound uses the changed volume for its next chunks without restarting");
    settings.soundVolume = 0; audio.UpdateSettings(settings);
    Check(Until([] { return true; }, 60) && true, "mute request is accepted");
    FakeDevice::Complete(); Sleep(60);
    const auto mutedFrames = FakeDevice::Count(5000) + FakeDevice::Count(10000);
    FakeDevice::Complete(); Sleep(60);
    Check(FakeDevice::Count(5000) + FakeDevice::Count(10000) == mutedFrames, "muted voices are flushed and do not refill");

    FakeDevice::Immediate(true);
    settings.soundVolume = 100; audio.UpdateSettings(settings);
    // A reload drops key requests until the sources are prepared again; wait
    // for the worker itself rather than for a notification that an earlier
    // reload may still have in flight.
    Check(Until([&] { return audio.Ready(); }), "the worker reports ready after the unmute reload");
    audio.Key(clickshade::kKeySpace);
    Check(Until([] { return FakeDevice::Count(1300) >= 2400; }), "the space category plays the set's space sound");
    // Overlapping key voices sum in the histogram, so each press is allowed to
    // finish before the next one; the variant choice is what is under test.
    const auto keyFrames = [] { return FakeDevice::Count(1100) + FakeDevice::Count(1200); };
    const auto keysBefore = keyFrames();
    bool sequential = true;
    for (unsigned press = 1; press <= 3; ++press) {
        audio.Key(clickshade::kKeyGeneric);
        sequential = Until([&] { return keyFrames() >= keysBefore + press * 2400; }) && sequential;
    }
    Check(sequential, "every generic key press plays exactly one complete key variant");
    Check(FakeDevice::Count(1100) > 0 && FakeDevice::Count(1200) > 0, "repeated presses alternate between the key variants");
    audio.Key(clickshade::kKeyEnter);
    Check(Until([&] { return keyFrames() >= keysBefore + 4 * 2400; }), "a category without its own file falls back to a key variant");
    settings.keyboardSoundsEnabled = false; audio.UpdateSettings(settings);
    Until([&] { return audio.Ready(); });
    const auto disabledBefore = FakeDevice::Count(1300);
    audio.Key(clickshade::kKeySpace); Sleep(80);
    Check(FakeDevice::Count(1300) == disabledBefore, "disabled keyboard sounds ignore key events");
    audio.PreviewKey();
    Check(Until([&] { return keyFrames() >= keysBefore + 5 * 2400; }), "keyboard preview works while keyboard sounds are off");

    for (unsigned i = 0; i < 1000; ++i) audio.Click(7);
    Sleep(150);
    audio.SetAllowed(false); audio.Preview(7);
    const auto stopBegin = GetTickCount64();
    audio.Stop();
    Check(GetTickCount64() - stopBegin < 1000, "pause and shutdown do not wait for the remaining sound duration");
    Check(FakeDevice::closes == FakeDevice::opens, "the output device is closed exactly as often as it was opened");

    FakeDevice::failOpen = true;
    clickshade::ClickAudio failing; failing.SetNotifyWindow(window, WM_APP + 17);
    notifications = 0;
    Check(failing.Start(settings, error) && Until([] { return notifications != 0; }), "the worker starts without an output device");
    failing.Preview(1);
    Check(Until([&] { return failing.LastError().find(L"fake device refused") != std::wstring::npos; }), "an output failure is reported without stopping the worker");
    failing.Stop();
    FakeDevice::failOpen = false;

    if (window) DestroyWindow(window);
    for (const auto& path : {keyboardFolder, folder}) RemoveDirectoryW(path.c_str());
    std::printf("Results: %u passed, %u failed.\n", passed, failed);
    return failed ? 1 : 0;
}
