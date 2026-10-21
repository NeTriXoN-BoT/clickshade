// Explicit interactive-desktop integration checks for the overlay renderer.
// Close ClickShade, then run visual_smoke.exe --run. This briefly moves the
// pointer and sends clicks inside its own test window. Screenshots are written
// to the current directory. Wine results do not establish native-game behavior.
#include "visual_effects.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr COLORREF kBackground = RGB(24, 32, 44);
constexpr COLORREF kStripe = RGB(230, 60, 50);
int passed = 0, failed = 0, skipped = 0;
bool wine = false;
unsigned leftClicks = 0, rightClicks = 0;
RECT stripe{}; // Client-area stripe the magnifier check looks for at twice its size.

void Check(bool condition, const char* description) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
    std::fflush(stdout);
    condition ? ++passed : ++failed;
}

void Require(bool condition, const char* description) {
    Check(condition, description);
    if (!condition) throw std::runtime_error(description);
}

void Skip(const char* description) {
    std::printf("SKIP %s\n", description);
    ++skipped;
}

void PumpFor(unsigned milliseconds) {
    const ULONGLONG until = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount64() >= until) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (true);
}

LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_LBUTTONDOWN: ++leftClicks; return 0;
    case WM_RBUTTONDOWN: ++rightClicks; return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU: return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(kBackground);
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        if (stripe.right > stripe.left) {
            HBRUSH mark = CreateSolidBrush(kStripe);
            FillRect(dc, &stripe, mark);
            DeleteObject(mark);
        }
        EndPaint(window, &paint);
        return 0;
    }
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}

class Host {
public:
    HWND window = nullptr;
    Host() {
        previousForeground_ = GetForegroundWindow();
        GetCursorPos(&previousPosition_);
        WNDCLASSW cls{};
        cls.lpfnWndProc = HostProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = L"ClickShade.VisualSmoke.Host";
        RegisterClassW(&cls);
        RECT bounds{0, 0, 760, 420};
        AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOOLWINDOW);
        window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName,
            L"ClickShade overlay visual check", WS_OVERLAPPEDWINDOW,
            80, 60, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, cls.hInstance, nullptr);
        if (!window) throw std::runtime_error("Test host creation failed");
        ShowWindow(window, SW_SHOWNORMAL);
        SetForegroundWindow(window);
        Move(180, 190);
        PumpFor(80);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    ~Host() {
        if (window) DestroyWindow(window);
        SetCursorPos(previousPosition_.x, previousPosition_.y);
        if (previousForeground_ && IsWindow(previousForeground_))
            SetForegroundWindow(previousForeground_);
    }
    POINT Move(int x, int y) const {
        POINT point{x, y};
        ClientToScreen(window, &point);
        SetCursorPos(point.x, point.y);
        return point;
    }
    RECT Bounds() const {
        RECT result{};
        GetClientRect(window, &result);
        POINT origin{};
        ClientToScreen(window, &origin);
        OffsetRect(&result, origin.x, origin.y);
        return result;
    }
private:
    HWND previousForeground_ = nullptr;
    POINT previousPosition_{};
};

struct OverlayQuery { std::vector<HWND> windows; bool visibleOnly = true; const wchar_t* className = L"ClickShade.VisualOverlay.v1"; };
BOOL CALLBACK FindOverlay(HWND window, LPARAM argument) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId()) return TRUE;
    auto& query = *reinterpret_cast<OverlayQuery*>(argument);
    wchar_t name[96]{};
    if (!GetClassNameW(window, name, 96) || std::wcscmp(name, query.className) != 0) return TRUE;
    if (!query.visibleOnly || IsWindowVisible(window)) query.windows.push_back(window);
    return TRUE;
}

std::vector<HWND> Overlays(bool visibleOnly = true) {
    OverlayQuery query;
    query.visibleOnly = visibleOnly;
    EnumWindows(FindOverlay, reinterpret_cast<LPARAM>(&query));
    return query.windows;
}

std::vector<HWND> LensHosts() {
    OverlayQuery query;
    query.className = L"ClickShade.VisualLens.v1";
    EnumWindows(FindOverlay, reinterpret_cast<LPARAM>(&query));
    return query.windows;
}

bool Above(HWND first, HWND second) {
    struct Order { HWND first, second; bool found = false; } order{first, second};
    EnumWindows([](HWND window, LPARAM argument) -> BOOL {
        auto& value = *reinterpret_cast<Order*>(argument);
        if (window == value.first) { value.found = true; return FALSE; }
        return window != value.second;
    }, reinterpret_cast<LPARAM>(&order));
    return order.found;
}

void CheckStacking(clickshade::VisualEffects& effects, const Host& host, POINT point) {
    const auto overlays = Overlays();
    Require(overlays.size() == 1, "stacking checks start with the existing highlight");
    const HWND overlay = overlays[0];
    HWND blocker = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"ClickShade.VisualSmoke.Host", L"ClickShade topmost stacking test", WS_POPUP,
        point.x - 70, point.y - 70, 140, 140, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(blocker != nullptr, "an independent topmost test surface is created");
    ShowWindow(blocker, SW_SHOWNOACTIVATE);
    PumpFor(110);
    Check(Above(overlay, blocker),
          "a newly shown topmost surface cannot leave a stationary highlight covered");
    SetWindowPos(blocker, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    PumpFor(110);
    if (wine && !Above(overlay, blocker))
        Skip("Wine does not emit topmost reorder/location WinEvents; real Windows notification delivery remains unverified");
    else Check(Above(overlay, blocker),
          "topmost reordering is repaired without a click, pointer move or foreground refresh");
    NotifyWinEvent(EVENT_OBJECT_REORDER, GetDesktopWindow(), OBJID_WINDOW, CHILDID_SELF);
    PumpFor(80);
    Check(Above(overlay, blocker),
          "a desktop reorder notification repairs stacking without pointer movement or a foreground change");
    Check(GetForegroundWindow() == host.window,
          "stacking repair never activates an effect or steals foreground focus");
    DestroyWindow(blocker);
    blocker = nullptr;
    HMENU popup = CreatePopupMenu();
    Require(popup != nullptr, "a real native context menu is created");
    AppendMenuW(popup, MF_STRING, 1, L"Context menu test item");
    AppendMenuW(popup, MF_STRING, 2, L"Another item");
    bool sawMenu = false, aboveMenu = false;
    std::thread observer([&] {
        Sleep(140);
        HWND menu = FindWindowW(L"#32768", nullptr);
        sawMenu = menu && IsWindowVisible(menu);
        aboveMenu = sawMenu && Above(overlay, menu);
        PostMessageW(host.window, WM_CANCELMODE, 0, 0);
    });
    TrackPopupMenuEx(popup, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
        point.x - 5, point.y - 5, host.window, nullptr);
    observer.join();
    DestroyMenu(popup);
    Check(sawMenu && aboveMenu, "the stationary highlight remains above a real native context menu");
    PumpFor(50);
    const auto settled = effects.Stats();
    PumpFor(150);
    Check(effects.Stats().frameCount == settled.frameCount && !effects.Stats().framePending,
          "stacking repair returns to an idle wait without a self-triggered event loop");
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    Check(GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) && cursor.hCursor,
          "the native cursor remains visible while the highlight is stationary");
    effects.SetAllowed(false);
    PumpFor(40);
    blocker = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"ClickShade.VisualSmoke.Host", L"ClickShade paused stacking test", WS_POPUP | WS_VISIBLE,
        point.x - 70, point.y - 70, 140, 140, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowPos(blocker, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    PumpFor(90);
    Check(Overlays().empty(), "stacking notifications cannot reshow effects while paused");
    DestroyWindow(blocker);
    effects.SetAllowed(true);
    PumpFor(50);
}

struct Screenshot {
    RECT bounds{};
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
    explicit Screenshot(RECT rectangle) : bounds(rectangle) {
        width = bounds.right - bounds.left;
        height = bounds.bottom - bounds.top;
        pixels.resize(static_cast<std::size_t>(width) * height);
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* address = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &address, nullptr, 0);
        HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
        const bool copied = screen && memory && bitmap && address &&
            BitBlt(memory, 0, 0, width, height, screen, bounds.left, bounds.top,
                   SRCCOPY | CAPTUREBLT);
        if (copied) {
            GdiFlush();
            std::copy_n(static_cast<const std::uint32_t*>(address), pixels.size(), pixels.data());
        }
        if (old) SelectObject(memory, old);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        if (!copied) throw std::runtime_error("Desktop capture failed");
    }
    COLORREF At(POINT point) const {
        const int x = point.x - bounds.left, y = point.y - bounds.top;
        if (x < 0 || y < 0 || x >= width || y >= height) return CLR_INVALID;
        const auto value = pixels[static_cast<std::size_t>(y) * width + x];
        return RGB((value >> 16) & 255, (value >> 8) & 255, value & 255);
    }
    bool Save(const wchar_t* path) const {
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = width;
        info.biHeight = -height;
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biSizeImage = static_cast<DWORD>(pixels.size() * 4);
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + info.biSizeImage;
        HANDLE output = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool okay = WriteFile(output, &file, sizeof(file), &written, nullptr) &&
            WriteFile(output, &info, sizeof(info), &written, nullptr) &&
            WriteFile(output, pixels.data(), info.biSizeImage, &written, nullptr);
        CloseHandle(output);
        return okay;
    }
};

int Difference(COLORREF a, COLORREF b) {
    return std::max({std::abs(static_cast<int>(GetRValue(a)) - GetRValue(b)),
                     std::abs(static_cast<int>(GetGValue(a)) - GetGValue(b)),
                     std::abs(static_cast<int>(GetBValue(a)) - GetBValue(b))});
}

unsigned ColorPixels(const Screenshot& shot, RECT area, bool left) {
    unsigned result = 0;
    for (int y = area.top; y < area.bottom; ++y) for (int x = area.left; x < area.right; ++x) {
        const COLORREF color = shot.At({x, y});
        if (color == CLR_INVALID) continue;
        const int red = GetRValue(color), blue = GetBValue(color);
        if (left ? (blue > red + 30 && blue > GetBValue(kBackground) + 20)
                 : (red > blue + 30 && red > GetRValue(kBackground) + 20)) ++result;
    }
    return result;
}

void SendClick(bool right) {
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_MOUSE;
    input[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    input[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    Require(SendInput(2, input, sizeof(INPUT)) == 2, "mouse click is injected into the test desktop");
    PumpFor(80);
}

double ClockSeconds() {
    LARGE_INTEGER value{}, frequency{};
    QueryPerformanceCounter(&value);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(value.QuadPart) / frequency.QuadPart;
}

int BlueRadius(const Screenshot& shot, POINT origin, int maximum) {
    int result = 0;
    for (int distance = 2; distance < maximum; ++distance) {
        const COLORREF pixel = shot.At({origin.x - distance, origin.y});
        if (static_cast<int>(GetBValue(pixel)) > static_cast<int>(GetRValue(pixel)) + 30 &&
            GetBValue(pixel) > GetBValue(kBackground) + 20) result = distance;
    }
    return result;
}

void CheckResponsiveRendering(clickshade::VisualEffects& effects,
                              clickshade::Settings& settings, const Host& host) {
    effects.SetAllowed(false);
    PumpFor(35);
    settings.highlightEnabled = true;
    settings.highlightStyle = clickshade::kHighlightRing;
    settings.highlightRadius = 40;
    settings.highlightThickness = 3;
    settings.clickEffectsEnabled = false;
    settings.visualRefreshRate = 240;
    effects.UpdateSettings(settings);
    effects.SetAllowed(true);
    const POINT origin = host.Move(300, 200);
    effects.PointerActivity();
    PumpFor(70);
    const auto initial = effects.Stats();
    Check(initial.targetRefreshRate == 240,
          "manual 240 Hz pacing takes effect after changing settings while paused");
    Check(initial.monitorDpi >= 96 && initial.monitorDpi <= 480,
          "effect geometry reports the active monitor DPI");
    const auto windows = Overlays();
    Require(windows.size() == 1, "high-refresh checks start with one cached highlight");
    RECT bounds{};
    GetWindowRect(windows[0], &bounds);
    const int radius = static_cast<int>((settings.highlightRadius * initial.monitorDpi + 48) / 96);
    Check(bounds.right - bounds.left == radius * 2 + 7 &&
          bounds.bottom - bounds.top == radius * 2 + 7,
          "DIP radius produces an odd, DPI-scaled surface with an exact pointer center");
    PumpFor(120);
    const auto idle = effects.Stats();
    Check(idle.frameCount == initial.frameCount && !idle.framePending,
          "stationary highlight schedules no repeating frame work");

    const auto beforeBurst = effects.Stats();
    for (int i = 0; i != 10000; ++i) effects.PointerActivity();
    const POINT final = host.Move(410, 220);
    effects.PointerActivity();
    PumpFor(65);
    const auto afterBurst = effects.Stats();
    GetWindowRect(windows[0], &bounds);
    Check((bounds.left + bounds.right) / 2 == final.x &&
          (bounds.top + bounds.bottom) / 2 == final.y,
          "coalesced movement renders the latest pointer position without a stale-position backlog");
    Check(afterBurst.highlightRasterizations == beforeBurst.highlightRasterizations,
          "high-rate motion reuses cached highlight pixels");
    Check(afterBurst.pointerWakeups - beforeBurst.pointerWakeups < 32 &&
          afterBurst.frameCount - beforeBurst.frameCount < 32,
          "ten thousand input notifications produce bounded, coalesced renderer work");
    Check(!afterBurst.framePending, "coalesced movement returns to an idle wait after its final frame");

    settings.highlightThickness = 12;
    effects.UpdateSettings(settings);
    PumpFor(65);
    const auto thick = Screenshot(host.Bounds());
    Check(Difference(thick.At({final.x - radius + 7, final.y}), kBackground) > 50,
          "outline thickness changes visibly without changing the outer radius");
    Check(Overlays().size() == 1 && Overlays()[0] == windows[0],
          "live appearance changes retain the existing overlay window");
    const auto beforeUnrelated = effects.Stats();
    settings.soundVolume = settings.soundVolume == 73 ? 72 : 73;
    effects.UpdateSettings(settings);
    PumpFor(45);
    Check(effects.Stats().highlightRasterizations == beforeUnrelated.highlightRasterizations,
          "unrelated settings do not rerasterize the highlight");

    settings.highlightEnabled = false;
    settings.clickEffectsEnabled = true;
    settings.leftClickEnabled = true;
    settings.clickStyle = clickshade::kClickRing;
    settings.clickRadius = 60;
    settings.clickThickness = 3;
    settings.clickDurationMs = 1600;
    settings.clickOpacity = 95;
    settings.leftClickColor = RGB(40, 150, 255);
    settings.clickEasing = 1;
    double measuredRates[3]{};
    const unsigned targets[] = {30, 60, 240};
    for (unsigned i = 0; i < 3; ++i) {
        effects.SetAllowed(false);
        PumpFor(25);
        settings.visualRefreshRate = static_cast<int>(targets[i]);
        effects.UpdateSettings(settings);
        effects.SetAllowed(true);
        host.Move(300, 200);
        PumpFor(30);
        effects.Click(1);
        PumpFor(45);
        const auto before = effects.Stats();
        const double start = ClockSeconds();
        PumpFor(400);
        const double elapsed = ClockSeconds() - start;
        const auto after = effects.Stats();
        const auto frames = after.frameCount - before.frameCount;
        measuredRates[i] = frames / elapsed;
        std::printf("INFO target=%u Hz, submitted=%llu frames in %.3f s (%.1f/s), high-resolution timer=%s\n",
                    targets[i], static_cast<unsigned long long>(frames), elapsed, measuredRates[i],
                    after.highResolutionTimer ? "yes" : "no");
        Check(after.targetRefreshRate == targets[i] && frames > 5 &&
              frames <= static_cast<std::uint64_t>(elapsed * targets[i]) + 3,
              "active click animation respects the selected bounded frame cadence");
    }
    if (effects.Stats().highResolutionTimer)
        Check(measuredRates[2] > measuredRates[1] * 2.0,
              "240 Hz mode submits materially more animation frames than 60 Hz mode");
    else Skip("platform lacks high-resolution waitable timers; actual high-refresh scheduling remains unverified");

    int radii[3]{};
    settings.clickDurationMs = 1000;
    settings.visualRefreshRate = 240;
    for (int easing = 0; easing != 3; ++easing) {
        effects.SetAllowed(false);
        PumpFor(25);
        settings.clickEasing = easing;
        effects.UpdateSettings(settings);
        effects.SetAllowed(true);
        host.Move(300, 200);
        PumpFor(25);
        effects.Click(1);
        PumpFor(205);
        const auto shot = Screenshot(host.Bounds());
        radii[easing] = BlueRadius(shot, origin, radius * 3);
    }
    std::printf("INFO click radii at approximately 205 ms: linear=%d, ease-out=%d, smoothstep=%d\n",
                radii[0], radii[1], radii[2]);
    Check(radii[1] > radii[0] + 5 && radii[0] > radii[2] + 2,
          "linear, cubic ease-out and smoothstep settings produce distinct visible expansion curves");
    const auto activeWindows = Overlays();
    settings.soundVolume = 70;
    effects.UpdateSettings(settings);
    PumpFor(35);
    Check(!activeWindows.empty() && Overlays() == activeWindows,
          "unrelated settings preserve an active click animation");
    effects.SetAllowed(false);
    PumpFor(45);
    const auto stopped = effects.Stats();
    PumpFor(100);
    Check(!stopped.framePending && effects.Stats().frameCount == stopped.frameCount,
          "rule suppression cancels the frame timer and all idle rendering work");

    settings.visualRefreshRate = 0;
    effects.UpdateSettings(settings);
    effects.SetAllowed(true);
    PumpFor(40);
    Check(effects.Stats().targetRefreshRate >= 30 && effects.Stats().targetRefreshRate <= 360 &&
          effects.Stats().motionRefreshRate >= effects.Stats().targetRefreshRate,
          "adaptive Auto stays in 30–360 Hz while cached motion retains the monitor cap");
}

class EventFlood {
public:
    EventFlood(HWND window, DWORD event, LONG object, unsigned durationMs)
        : worker_([this, window, event, object, durationMs] {
            const ULONGLONG until = GetTickCount64() + durationMs;
            while (!stop_.load() && GetTickCount64() < until) {
                NotifyWinEvent(event, window, object, CHILDID_SELF);
                count_.fetch_add(1, std::memory_order_relaxed);
            }
        }) {}
    ~EventFlood() { Stop(); }
    void Stop() { stop_.store(true); if (worker_.joinable()) worker_.join(); }
    unsigned Count() const { return count_.load(std::memory_order_relaxed); }
private:
    std::atomic<bool> stop_{false};
    std::atomic<unsigned> count_{0};
    std::thread worker_;
};

void CheckEventIsolation(clickshade::VisualEffects& effects, clickshade::Settings& settings,
                         const Host& host) {
    effects.SetAllowed(false); PumpFor(35);
    settings.enabled = true;
    settings.highlightEnabled = true;
    settings.clickEffectsEnabled = settings.trailEnabled = false;
    settings.visualRefreshRate = 240;
    effects.UpdateSettings(settings); effects.SetAllowed(true); PumpFor(70);
    auto overlays = Overlays();
    Require(overlays.size() == 1, "event-pressure check starts with a single cached highlight");
    const HWND highlight = overlays[0];
    const auto before = effects.Stats();
    unsigned samples = 0, stale = 0;
    EventFlood flood(host.window, EVENT_OBJECT_LOCATIONCHANGE, OBJID_CLIENT, 1000);
    const ULONGLONG started = GetTickCount64();
    POINT final{};
    while (GetTickCount64() - started < 650) {
        final = host.Move(120 + static_cast<int>(samples % 30) * 16,
                          175 + static_cast<int>(samples % 3) * 12);
        effects.PointerActivity();
        PumpFor(8);
        RECT bounds{};
        if (!GetWindowRect(highlight, &bounds) ||
            std::abs((bounds.left + bounds.right) / 2 - final.x) > 2 ||
            std::abs((bounds.top + bounds.bottom) / 2 - final.y) > 2) ++stale;
        ++samples;
    }
    flood.Stop();
    const auto after = effects.Stats();
    std::printf("INFO irrelevant WinEvents=%u, motion samples=%u, stale samples=%u, frames=%llu\n",
                flood.Count(), samples, stale,
                static_cast<unsigned long long>(after.frameCount - before.frameCount));
    Check(flood.Count() > 1000 && samples > 20, "stress fixture emits a substantial global accessibility-event burst");
    Check(stale * 3 < samples && after.frameCount - before.frameCount >= samples * 2 / 3,
          "irrelevant desktop event traffic cannot starve latest-position highlight updates");
    PumpFor(60);
    RECT finalBounds{}; GetWindowRect(highlight, &finalBounds);
    Check((finalBounds.left + finalBounds.right) / 2 == final.x &&
          (finalBounds.top + finalBounds.bottom) / 2 == final.y,
          "the final fast-motion position is retained after the event burst ends");
    Check(after.highlightRasterizations == before.highlightRasterizations,
          "event pressure never rebuilds cached highlight artwork");
    const auto beforeReorder = effects.Stats();
    EventFlood reorder(GetDesktopWindow(), EVENT_OBJECT_REORDER, OBJID_WINDOW, 500);
    PumpFor(330);
    effects.SetAllowed(false); PumpFor(65);
    Check(Overlays().empty(), "pause hides feedback promptly while relevant stacking events keep arriving");
    reorder.Stop(); PumpFor(50);
    Check(effects.Stats().stackWakeups - beforeReorder.stackWakeups <= 18,
          "continuous relevant notifications coalesce to a bounded stacking-repair rate");
    const auto settled = effects.Stats(); PumpFor(100);
    Check(effects.Stats().frameCount == settled.frameCount && !effects.Stats().framePending,
          "event pressure leaves no idle frame loop after pause");
    effects.SetAllowed(true); PumpFor(50);
    EventFlood shutdown(host.window, EVENT_OBJECT_LOCATIONCHANGE, OBJID_CLIENT, 1500);
    PumpFor(60);
    const double stopStarted = ClockSeconds();
    effects.Stop();
    const double stopSeconds = ClockSeconds() - stopStarted;
    shutdown.Stop();
    Check(stopSeconds < 0.75 && Overlays(false).empty(),
          "shutdown unsubscribes and joins safely while accessibility traffic is active");
    std::wstring error;
    Require(effects.Start(settings, error), "renderer can restart after the event-pressure shutdown");
    effects.SetAllowed(true); PumpFor(50);
    Check(Overlays().size() == 1 && effects.LastError().empty(),
          "restart recreates healthy passive feedback after receiver cleanup");
}

void CheckTrails(clickshade::VisualEffects& effects, clickshade::Settings& settings,
                 const Host& host) {
    effects.SetAllowed(false);
    PumpFor(30);
    settings.highlightEnabled = false;
    settings.clickEffectsEnabled = false;
    settings.trailEnabled = true;
    settings.trailRadius = 8;
    settings.trailOpacity = 90;
    settings.trailDurationMs = 500;
    settings.trailColor = RGB(120, 226, 194);
    settings.visualRefreshRate = 240;
    for (int style = 0; style < 5; ++style) {
        settings.trailStyle = style;
        effects.UpdateSettings(settings);
        effects.SetAllowed(true);
        host.Move(120, 160);
        effects.PointerActivity();
        PumpFor(30);
        Check(Overlays().empty(), "a newly enabled trail does not leave a stamp on a stationary pointer");
        POINT end{};
        for (int i = 0; i < 18; ++i) {
            end = host.Move(140 + i * 27, 165 + (i % 6) * 9);
            effects.PointerActivity();
            PumpFor(18);
        }
        const auto stamps = Overlays();
        Check(stamps.size() > 4 && stamps.size() <= 32,
              "each trail style produces visible stamps in a strictly bounded pool");
        POINT actual{};
        Check(GetCursorPos(&actual) && actual.x == end.x && actual.y == end.y,
              "trail rendering preserves the actual pointer coordinates exactly");
        Check(GetForegroundWindow() == host.window,
              "trail stamps preserve the underlying application's foreground focus");
        const wchar_t* images[]{L"visual-qa-trail-dots.bmp", L"visual-qa-trail-rings.bmp",
                                L"visual-qa-trail-sparkles.bmp", L"visual-qa-trail-comet.bmp",
                                L"visual-qa-trail-fine-sparkle.bmp"};
        Check(Screenshot(host.Bounds()).Save(images[style]), "trail style screenshot is saved");
        PumpFor(570);
        Check(Overlays().empty() && !effects.Stats().framePending,
              "all trail stamps expire and stop their frame timer after movement ends");
        const auto idle = effects.Stats();
        PumpFor(80);
        Check(effects.Stats().frameCount == idle.frameCount,
              "expired trails perform no continuing idle redraws");
        effects.SetAllowed(false);
        PumpFor(25);
    }
    const int shapeRadius=static_cast<int>((12*effects.Stats().monitorDpi+48)/96);
    const int shapeExtent=shapeRadius*2+8, movement=shapeRadius*5+64;
    struct Shape { unsigned pixels=0; int left=1000,right=-1000,top=1000,bottom=-1000; };
    const auto shape=[&](int style,int dx,int dy) {
        effects.SetAllowed(false); PumpFor(25);
        settings.trailStyle=style; settings.trailRadius=12;
        settings.trailOpacity=100; settings.trailDurationMs=700;
        effects.UpdateSettings(settings);
        const POINT center=host.Move(320,220);
        effects.SetAllowed(true); effects.PointerActivity(); PumpFor(30);
        host.Move(320+dx,220+dy); effects.PointerActivity(); PumpFor(24);
        Screenshot picture({center.x-shapeExtent,center.y-shapeExtent,center.x+shapeExtent+1,center.y+shapeExtent+1});
        Shape result;
        // Color dominance works both for native compositing and Wine's
        // captured premultiplied layered pixels; black/host pixels are ignored.
        for(int y=-shapeExtent;y<=shapeExtent;++y) for(int x=-shapeExtent;x<=shapeExtent;++x) {
            const COLORREF color=picture.At({center.x+x,center.y+y});
            if(GetGValue(color)>GetRValue(color)+12 && GetBValue(color)>GetRValue(color)+8 && GetGValue(color)>35) {
                ++result.pixels; result.left=std::min(result.left,x); result.right=std::max(result.right,x);
                result.top=std::min(result.top,y); result.bottom=std::max(result.bottom,y);
            }
        }
        return result;
    };
    const Shape broad=shape(2,movement,0),fine=shape(4,movement,0);
    Check(fine.pixels>8&&fine.pixels*100<broad.pixels*85,
          "Fine sparkle has a visibly thinner four-point silhouette than Sparkles");
    const Shape right=shape(3,movement,0),left=shape(3,-movement,0),down=shape(3,0,movement);
    Check(right.pixels>8&&right.left<-shapeRadius&&right.right<shapeRadius*.84&&
          left.right>shapeRadius&&left.left>-shapeRadius*.84,
          "Comet reverses its tapered tail with the actual horizontal movement direction");
    Check(down.pixels>8&&down.top<-shapeRadius&&down.bottom<shapeRadius*.84&&
          down.right-down.left<shapeRadius*1.5,
          "Comet rotates its bounded geometry with actual vertical movement");
    effects.SetAllowed(false); PumpFor(25);
    effects.SetAllowed(true);
    host.Move(100, 160); effects.PointerActivity(); PumpFor(30);
    host.Move(150, 160); effects.PointerActivity(); PumpFor(30);
    effects.SetAllowed(false); PumpFor(30);
    host.Move(200, 160); effects.PointerActivity(); PumpFor(30);
    Check(Overlays().empty(), "pause immediately clears trails and blocks new stamps");
    settings.trailEnabled = false;
    effects.UpdateSettings(settings);
}

unsigned LightPixels(const Screenshot& shot, RECT area) {
    unsigned result = 0;
    for (int y = area.top; y < area.bottom; ++y) for (int x = area.left; x < area.right; ++x) {
        const COLORREF color = shot.At({x, y});
        if (color != CLR_INVALID && GetRValue(color) > 200 && GetGValue(color) > 200 && GetBValue(color) > 200) ++result;
    }
    return result;
}

void CheckKeyDisplay(clickshade::VisualEffects& effects, clickshade::Settings& settings, const Host& host) {
    effects.SetAllowed(false); PumpFor(30);
    settings.highlightEnabled = settings.clickEffectsEnabled = settings.trailEnabled = settings.magnifierEnabled = false;
    settings.keyDisplayEnabled = true;
    settings.keyDisplayMode = 1;
    settings.keyDisplayDurationMs = 600;
    settings.keyDisplayPosition = 0;
    settings.keyDisplaySize = 100;
    settings.keyDisplayOpacity = 90;
    effects.UpdateSettings(settings);
    effects.SetAllowed(true);
    host.Move(200, 200);
    PumpFor(50);
    Check(Overlays().empty(), "an enabled key display shows nothing until a key is pressed");
    effects.Key('A', 0x1e, false, 0);
    PumpFor(90);
    auto panels = Overlays();
    Require(panels.size() == 1, "a key press shows one key display window");
    RECT bounds{};
    GetWindowRect(panels[0], &bounds);
    const int firstWidth = bounds.right - bounds.left;
    const int firstHeight = bounds.bottom - bounds.top;
    std::printf("INFO key display for one key: %ld x %ld px\n", bounds.right - bounds.left, bounds.bottom - bounds.top);
    Check(firstWidth > 20 && firstHeight > 20 && firstHeight < 200, "the key display starts as one compact row");
    const auto stats = effects.Stats();
    Check(stats.keyDisplayVisible && stats.keyDisplayUpdates >= 1, "statistics report the visible key display");
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(host.window, MONITOR_DEFAULTTONEAREST), &monitor);
    Check(bounds.bottom <= monitor.rcWork.bottom && bounds.top > monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top) / 2 &&
          std::abs((bounds.left + bounds.right) / 2 - (monitor.rcWork.left + monitor.rcWork.right) / 2) < 8,
          "the bottom-center position sits above the taskbar in the middle of the active monitor");
    {
        const Screenshot shot(bounds);
        const unsigned light = LightPixels(shot, bounds);
        Check(light > 10, "the label text is drawn in a light color on the dark backdrop");
        Check(shot.Save(L"visual-qa-key-display.bmp"), "key display screenshot is saved");
    }
    Check(GetForegroundWindow() == host.window, "the key display never takes foreground focus");
    effects.Key('A', 0x1e, false, 0);
    PumpFor(60);
    panels = Overlays();
    GetWindowRect(panels.empty() ? nullptr : panels[0], &bounds);
    Check(panels.size() == 1 && bounds.bottom - bounds.top == firstHeight && bounds.right - bounds.left > firstWidth,
          "typed characters join the same row");
    effects.Key('S', 0x1f, false, clickshade::kModControl | clickshade::kModShift);
    PumpFor(90);
    panels = Overlays();
    GetWindowRect(panels.empty() ? nullptr : panels[0], &bounds);
    Check(panels.size() == 1 && bounds.bottom - bounds.top > firstHeight + 20,
          "a shortcut starts a new row in the same window instead of opening more windows");
    PumpFor(600 + 350 + 200);
    Check(Overlays().empty() && !effects.Stats().framePending && !effects.Stats().keyDisplayVisible,
          "labels fade out after the hold time and the frame timer stops");
    effects.SetAllowed(false, true);
    PumpFor(40);
    effects.Key(VK_F5, 0x3f, false, 0);
    PumpFor(90);
    Check(Overlays().size() == 1, "the key display keeps working while a hidden pointer pauses the pointer effects");
    effects.SetAllowed(false, false);
    PumpFor(60);
    Check(Overlays().empty(), "pausing the key display hides its row immediately");
    effects.SetAllowed(true);
    settings.keyDisplayEnabled = false;
    effects.UpdateSettings(settings);
    PumpFor(40);
    effects.Key('A', 0x1e, false, 0);
    PumpFor(60);
    Check(Overlays().empty(), "a switched-off key display ignores key events");
    effects.PreviewKey();
    PumpFor(90);
    Check(Overlays().size() == 1, "the sample label shows while the switch is off");
    PumpFor(600 + 350 + 200);
    Check(Overlays().empty(), "the sample label fades like a real one");
}

unsigned Luma(COLORREF color) {
    return (GetRValue(color) * 299u + GetGValue(color) * 587u + GetBValue(color) * 114u) / 1000u;
}

bool PointerShown() {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    return GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) && !(cursor.flags & CURSOR_SUPPRESSED) && cursor.hCursor;
}

void CheckSpotlight(clickshade::VisualEffects& effects, clickshade::Settings& settings, const Host& host) {
    effects.SetAllowed(false); PumpFor(30);
    settings.clickEffectsEnabled = settings.trailEnabled = settings.magnifierEnabled = settings.keyDisplayEnabled = false;
    settings.highlightEnabled = true;
    settings.highlightStyle = clickshade::kHighlightSpotlight;
    settings.spotlightDim = 70;
    settings.spotlightRadius = 60;
    effects.UpdateSettings(settings);
    effects.SetAllowed(true);
    const POINT center = host.Move(260, 210);
    effects.PointerActivity();
    PumpFor(150);
    if (!PointerShown()) {
        Skip("the pointer is hidden or suppressed on this desktop (typing, touch or pen input); spotlight rendering remains unverified here");
        settings.highlightEnabled = false;
        effects.UpdateSettings(settings);
        PumpFor(40);
        return;
    }
    auto windows = Overlays();
    Require(windows.size() == 1, "the spotlight is one overlay window, so the dim area and the circle always move together");
    RECT bounds{};
    GetWindowRect(windows[0], &bounds);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &monitor);
    Check(EqualRect(&bounds, &monitor.rcMonitor) != FALSE, "the spotlight covers exactly the pointer's monitor");
    const HWND layer = windows[0];
    {
        const Screenshot shot(host.Bounds());
        const unsigned inside = Luma(shot.At(center));
        const unsigned outside = Luma(shot.At({center.x + 200, center.y}));
        std::printf("INFO spotlight luma: inside %u, outside %u\n", inside, outside);
        // A 70% dim leaves about a third of the brightness outside the circle.
        Check(inside >= outside * 2 + 6, "the circle at the pointer is clear while the rest of the screen is dimmed");
        Check(shot.Save(L"visual-qa-spotlight.bmp"), "spotlight screenshot is saved");
    }
    {
        const auto before = effects.Stats();
        PumpFor(400);
        const auto after = effects.Stats();
        std::printf("INFO resting spotlight: %llu frames and %llu stacking wakeups in 400 ms\n",
                    static_cast<unsigned long long>(after.frameCount - before.frameCount),
                    static_cast<unsigned long long>(after.stackWakeups - before.stackWakeups));
        Check(after.frameCount <= before.frameCount + 2 && !after.framePending,
              "a resting spotlight schedules no further frames (raising it does not feed its own stacking repair)");
    }
    const POINT moved = host.Move(520, 230);
    effects.PointerActivity();
    PumpFor(90);
    windows = Overlays();
    Check(windows.size() == 1 && windows[0] == layer, "moving the pointer moves the circle inside the same window");
    {
        const Screenshot shot(host.Bounds());
        Check(Luma(shot.At(moved)) >= Luma(shot.At(center)) * 2 + 6, "the circle follows the pointer and its old place is dimmed again");
    }
    effects.SetAllowed(false, true);
    PumpFor(60);
    Check(Overlays().size() == 1, "the spotlight stays in place while an application hides the pointer for typing");
    effects.SetAllowed(false, false);
    PumpFor(60);
    Check(Overlays().empty(), "pausing the effects hides the spotlight in one step");
    effects.SetAllowed(true);
    effects.PointerActivity();
    PumpFor(90);
    Check(Overlays().size() == 1, "the spotlight returns when the effects are allowed again");
    settings.highlightStyle = clickshade::kHighlightRing;
    effects.UpdateSettings(settings);
    PumpFor(90);
    bool screenSized = false;
    for (HWND window : Overlays(false)) {
        RECT rect{};
        GetWindowRect(window, &rect);
        if (rect.right - rect.left >= monitor.rcMonitor.right - monitor.rcMonitor.left) screenSized = true;
    }
    Check(!screenSized, "switching to another highlight style frees the monitor-sized spotlight window");
    settings.highlightEnabled = false;
    effects.UpdateSettings(settings);
    PumpFor(60);
}

void CheckMagnifier(clickshade::VisualEffects& effects, clickshade::Settings& settings, const Host& host) {
    effects.SetAllowed(false); PumpFor(30);
    settings.highlightEnabled = settings.clickEffectsEnabled = settings.trailEnabled = settings.keyDisplayEnabled = false;
    settings.magnifierEnabled = true;
    settings.magnifierZoom = 200;
    settings.magnifierSize = 200;
    settings.magnifierShape = 0;
    settings.magnifierPosition = 0;
    effects.UpdateSettings(settings);
    effects.SetAllowed(true);
    const POINT center = host.Move(380, 210);
    effects.PointerActivity();
    // The first lens creates the Magnification API window, which takes a
    // noticeable fraction of a second on some systems.
    for (int wait = 0; wait < 100 && (LensHosts().empty() || !effects.Stats().magnifierUpdates); ++wait) PumpFor(10);
    PumpFor(60);
    if (LensHosts().empty()) {
        const auto error = effects.LastError();
        const auto stats = effects.Stats();
        CURSORINFO cursor{};
        cursor.cbSize = sizeof(cursor);
        const bool pointerShown = GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) && !(cursor.flags & CURSOR_SUPPRESSED) && cursor.hCursor;
        std::printf("INFO no lens host yet: overlays=%zu updates=%llu visible=%s pointer flags=0x%lx error=\"%ls\"\n", Overlays().size(),
                    static_cast<unsigned long long>(stats.magnifierUpdates), stats.magnifierVisible ? "yes" : "no",
                    static_cast<unsigned long>(cursor.flags), error.c_str());
        const bool unavailable = error.find(L"magnifier") != std::wstring::npos || error.find(L"Magnification") != std::wstring::npos || wine;
        if (unavailable || !pointerShown) {
            // Typing or touch input suppresses the pointer, which pauses every
            // pointer-following effect by design; that is not a lens defect.
            Skip(unavailable ? "the Windows Magnification API is unavailable in this environment; lens rendering remains unverified here"
                             : "the pointer is hidden or suppressed on this desktop (typing, touch or pen input); lens rendering remains unverified here");
            settings.magnifierEnabled = false;
            effects.UpdateSettings(settings);
            PumpFor(40);
            return;
        }
    }
    auto hosts = LensHosts();
    Require(hosts.size() == 1, "an enabled magnifier shows one lens window at the pointer");
    RECT bounds{};
    GetWindowRect(hosts[0], &bounds);
    const int expected = static_cast<int>((200 * effects.Stats().monitorDpi + 48) / 96);
    Check(bounds.right - bounds.left == expected && bounds.bottom - bounds.top == expected,
          "the lens size follows the DIP setting and the monitor DPI");
    Check(std::abs((bounds.left + bounds.right) / 2 - center.x) <= 1 && std::abs((bounds.top + bounds.bottom) / 2 - center.y) <= 1,
          "the centered lens is placed on the pointer");
    Check(effects.Stats().magnifierVisible && effects.Stats().magnifierUpdates >= 1, "statistics report the visible lens");
    Check(Overlays().size() == 1, "the lens has exactly one frame overlay and no other pointer effect");
    // A stripe 30..50 px right of the pointer must appear 60..100 px right of
    // the lens center at 2x, while the real stripe position is covered.
    POINT client = center;
    ScreenToClient(host.window, &client);
    stripe = {client.x + 30, client.y - 70, client.x + 50, client.y + 70};
    InvalidateRect(host.window, nullptr, FALSE);
    UpdateWindow(host.window);
    PumpFor(160);
    {
        const Screenshot shot(bounds);
        const COLORREF magnified = shot.At({center.x + 80, center.y});
        const COLORREF original = shot.At({center.x + 40, center.y});
        const COLORREF between = shot.At({center.x + 15, center.y});
        std::printf("INFO lens pixels: at +80 %u,%u,%u; at +40 %u,%u,%u; at +15 %u,%u,%u\n",
                    GetRValue(magnified), GetGValue(magnified), GetBValue(magnified), GetRValue(original), GetGValue(original), GetBValue(original),
                    GetRValue(between), GetGValue(between), GetBValue(between));
        Check(GetRValue(magnified) > GetBValue(magnified) + 60, "content 40 px from the pointer appears 80 px from the lens center at 2x");
        Check(GetRValue(original) < GetBValue(original) + 30 && GetRValue(between) < GetBValue(between) + 30,
              "the lens shows enlarged content where the original stripe would be");
        Check(shot.Save(L"visual-qa-magnifier.bmp"), "magnifier screenshot is saved");
    }
    const unsigned before = leftClicks;
    SendClick(false);
    Check(leftClicks == before + 1, "a click passes through the centered lens to the window below");
    Check(GetForegroundWindow() == host.window, "the lens never takes foreground focus");
    settings.magnifierPosition = 1;
    settings.magnifierShape = 1;
    effects.UpdateSettings(settings);
    PumpFor(150);
    hosts = LensHosts();
    GetWindowRect(hosts.empty() ? nullptr : hosts[0], &bounds);
    Check(hosts.size() == 1 && !PtInRect(&bounds, center), "the beside placement keeps the lens off the pointer");
    const auto refreshed = effects.Stats();
    PumpFor(200);
    Check(effects.Stats().magnifierUpdates > refreshed.magnifierUpdates, "a resting lens keeps refreshing so live content stays current");
    stripe = {};
    InvalidateRect(host.window, nullptr, FALSE);
    settings.magnifierEnabled = false;
    effects.UpdateSettings(settings);
    PumpFor(80);
    Check(LensHosts().empty() && Overlays().empty() && !effects.Stats().framePending && !effects.Stats().magnifierVisible,
          "switching the magnifier off hides the lens and stops its refresh timer");
}

void RunChecks() {
    Host host;
    Require(GetForegroundWindow() == host.window, "ordinary test window owns foreground focus");
    clickshade::VisualEffects effects;
    clickshade::Settings settings;
    Check(!settings.trailEnabled, "cursor trails are disabled by default");
    int mouseParameters[3]{}, pointerSpeed = 0;
    const bool haveMouseParameters = SystemParametersInfoW(SPI_GETMOUSE, 0, mouseParameters, 0) &&
        SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &pointerSpeed, 0);
    settings.enabled = true;
    settings.highlightEnabled = true;
    settings.highlightStyle = clickshade::kHighlightFilled;
    settings.highlightRadius = 40;
    settings.highlightOpacity = 60;
    settings.highlightColor = RGB(250, 200, 40);
    settings.clickEffectsEnabled = false;
    std::wstring error;
    Require(effects.Start(settings, error), "renderer starts successfully");
    PumpFor(40);
    Check(Overlays().empty(), "startup stays hidden until foreground rules explicitly allow effects");
    const POINT first = host.Move(180, 190);
    effects.SetAllowed(true);
    effects.PointerActivity();
    PumpFor(100);
    auto overlays = Overlays();
    Require(overlays.size() == 1, "filled highlight creates one visible small overlay");
    constexpr LONG_PTR required = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                  WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    Check((GetWindowLongPtrW(overlays[0], GWL_EXSTYLE) & required) == required,
          "actual overlay has topmost, layered, transparent, noactivate and toolwindow styles");
    RECT overlayBounds{};
    GetWindowRect(overlays[0], &overlayBounds);
    Check(overlayBounds.right - overlayBounds.left < 420 &&
          overlayBounds.bottom - overlayBounds.top < 420,
          "highlight uses a bounded small window rather than a screen-sized surface");
    const auto filled = Screenshot(host.Bounds());
    const POINT interior{first.x - 10, first.y};
    const COLORREF expectedFill = RGB(160, 133, 42);
    const COLORREF actualFill = filled.At(interior);
    const COLORREF expectedPremultiplied = RGB(150, 120, 24);
    if (wine && Difference(actualFill, expectedPremultiplied) <= 3) {
        Check(true, "Wine capture exposes the correct color premultiplied by configured opacity");
        Skip("native desktop alpha compositing is not available in this Wine/Xvfb capture");
    } else {
        Check(Difference(actualFill, expectedFill) <= 4,
              "filled highlight is visibly composited at the configured color and opacity");
    }
    Check(filled.Save(L"visual-qa-filled.bmp"), "filled highlight screenshot is saved");
    Check(GetForegroundWindow() == host.window, "showing the highlight preserves foreground focus");

    SendClick(false);
    SendClick(true);
    Check(leftClicks == 1 && rightClicks == 1,
          "both real mouse clicks pass through the visible highlight to the underlying window");
    Check(GetForegroundWindow() == host.window, "click-through does not activate the overlay");

    settings.highlightStyle = clickshade::kHighlightRing;
    effects.UpdateSettings(settings);
    PumpFor(100);
    const auto outline = Screenshot(host.Bounds());
    const int radius = (overlayBounds.right - overlayBounds.left - 6) / 2;
    Check(Difference(outline.At(interior), kBackground) <= 2,
          "outline highlight leaves its interior fully transparent");
    Check(Difference(outline.At({first.x + radius - 1, first.y}), kBackground) > 50,
          "outline highlight visibly renders its colored outer ring");
    Check(outline.Save(L"visual-qa-outline.bmp"), "outline highlight screenshot is saved");

    const POINT moved = host.Move(280, 210);
    effects.PointerActivity();
    PumpFor(80);
    overlays = Overlays();
    Require(overlays.size() == 1, "movement reuses the single highlight window");
    GetWindowRect(overlays[0], &overlayBounds);
    Check((overlayBounds.left + overlayBounds.right) / 2 == moved.x &&
          (overlayBounds.top + overlayBounds.bottom) / 2 == moved.y,
          "cached highlight geometry follows the actual pointer position");

    CheckStacking(effects, host, moved);

    effects.SetAllowed(false);
    PumpFor(80);
    Check(Overlays().empty(), "SetAllowed(false) immediately hides every visual effect");
    effects.PointerActivity(); effects.Click(3);
    PumpFor(40);
    Check(Overlays().empty(), "input cannot reshow effects while rules disallow them");

    settings.highlightEnabled = false;
    settings.clickEffectsEnabled = true;
    settings.leftClickEnabled = settings.rightClickEnabled = true;
    settings.clickStyle = clickshade::kClickRing;
    settings.clickRadius = 46;
    settings.clickDurationMs = 1200;
    settings.clickOpacity = 95;
    settings.leftClickColor = RGB(40, 150, 255);
    settings.rightClickColor = RGB(255, 70, 125);
    effects.UpdateSettings(settings);
    effects.SetAllowed(true);
    PumpFor(40);
    const POINT left = host.Move(180, 210);
    effects.Click(1);
    PumpFor(60);
    const POINT right = host.Move(420, 210);
    effects.Click(2);
    PumpFor(80);
    overlays = Overlays();
    Check(overlays.size() == 2, "left and right button presses create separate active click effects");
    const auto clicks = Screenshot(host.Bounds());
    const RECT leftArea{left.x - 70, left.y - 70, left.x + 70, left.y + 70};
    const RECT rightArea{right.x - 70, right.y - 70, right.x + 70, right.y + 70};
    Check(ColorPixels(clicks, leftArea, true) > 50,
          "left click pulse visibly uses the selected blue color at its click origin");
    Check(ColorPixels(clicks, rightArea, false) > 50,
          "right click pulse visibly uses the selected pink color at its click origin");
    Check(Difference(clicks.At({left.x - 5, left.y}), kBackground) <= 2 &&
          Difference(clicks.At({right.x - 5, right.y}), kBackground) <= 2,
          "outline click pulses leave their centers transparent");
    Check(clicks.Save(L"visual-qa-clicks.bmp"), "independent left/right pulse screenshot is saved");
    Check(GetForegroundWindow() == host.window, "click animations preserve foreground focus");

    settings.clickStyle = clickshade::kClickPulse;
    effects.UpdateSettings(settings);
    PumpFor(60);
    const POINT filledOrigin = host.Move(300, 210);
    effects.Click(1);
    PumpFor(80);
    const auto filledClick = Screenshot(host.Bounds());
    Check(Difference(filledClick.At({filledOrigin.x - 5, filledOrigin.y}), kBackground) > 60,
          "filled click style visibly fills the pulse center");
    Check(filledClick.Save(L"visual-qa-click-filled.bmp"), "filled click screenshot is saved");
    PumpFor(1250);
    Check(Overlays().empty(), "completed click animations hide their windows");

    // Target: a contracting ring with inward ticks on the axes. Along an axis
    // the first colored pixel is a tick, well inside the ring met on a diagonal.
    settings.clickStyle = clickshade::kClickTarget;
    effects.UpdateSettings(settings);
    PumpFor(60);
    const POINT targetOrigin = host.Move(300, 210);
    effects.Click(1);
    PumpFor(80);
    const auto target = Screenshot(host.Bounds());
    const auto firstColored = [&](int dx, int dy) {
        for (int step = 2; step < 140; ++step)
            if (Difference(target.At({targetOrigin.x + dx * step, targetOrigin.y + dy * step}), kBackground) > 40) return step;
        return 0;
    };
    const int axisTick = firstColored(1, 0), diagonalRing = firstColored(1, 1);
    Check(Difference(target.At({targetOrigin.x - 5, targetOrigin.y}), kBackground) <= 2,
          "the target click keeps the pointer tip clear");
    Check(axisTick > 0 && diagonalRing > 0 && axisTick < diagonalRing * 1.41421 * 0.8,
          "the target click draws inward ticks on its axes, inside its ring");
    Check(target.Save(L"visual-qa-click-target.bmp"), "target click screenshot is saved");
    PumpFor(1250);
    Check(Overlays().empty(), "a completed target click hides its window");

    for (int i = 0; i < 20; ++i) effects.Click((i & 1) ? 1u : 2u);
    PumpFor(80);
    Check(Overlays().size() == 8, "rapid clicks are bounded to eight active particle windows");
    effects.SetAllowed(false);
    PumpFor(60);
    Check(Overlays().empty(), "rule suppression also hides all active click particles");
    CheckResponsiveRendering(effects, settings, host);
    CheckTrails(effects, settings, host);
    CheckSpotlight(effects, settings, host);
    CheckKeyDisplay(effects, settings, host);
    CheckMagnifier(effects, settings, host);
    CheckEventIsolation(effects, settings, host);
    settings.enabled = false;
    effects.UpdateSettings(settings);
    effects.SetAllowed(true);
    effects.Click(3);
    PumpFor(80);
    Check(Overlays().empty(), "master disable suppresses both effect types even when allowed");
    Check(effects.LastError().empty(), "no asynchronous graphics error occurred");
    effects.Stop();
    Check(Overlays(false).empty(), "shutdown destroys all overlay windows");
    Check(GetForegroundWindow() == host.window, "shutdown preserves the underlying window focus");
    if (haveMouseParameters) {
        int afterParameters[3]{}, afterSpeed = 0;
        Check(SystemParametersInfoW(SPI_GETMOUSE, 0, afterParameters, 0) &&
              SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &afterSpeed, 0) &&
              std::equal(std::begin(mouseParameters), std::end(mouseParameters), std::begin(afterParameters)) &&
              pointerSpeed == afterSpeed,
              "effects leave the user's mouse acceleration and pointer-speed settings unchanged");
    } else Skip("the test desktop cannot report mouse acceleration/speed settings");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || std::wcscmp(argv[1], L"--run") != 0) {
        std::puts("Usage: visual_smoke.exe --run\nClose ClickShade before testing.\n"
                  "The test moves the pointer and clicks inside its own window.");
        return 2;
    }
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ClickShade.Instance.v1");
    if (running) {
        CloseHandle(running);
        std::puts("Close the running ClickShade app first.");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    std::puts(wine ? "Platform: Wine; native Windows and game behavior remain unverified."
                   : "Platform: native Windows interactive desktop.");
    try { RunChecks(); }
    catch (const std::exception& error) { std::printf("ERROR %s\n", error.what()); ++failed; }
    std::printf("RESULT %d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
