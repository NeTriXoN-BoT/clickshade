// Interactive integration checks. This temporarily moves the mouse, clips it,
// hides it, and changes the test window to fullscreen. Use a test desktop.
#include "runtime.h"
#include "input_access.h"

#include <windows.h>
#include <array>
#include <cstdio>
#include <cwchar>
#include <cmath>
#include <string>
#include <vector>

namespace {
int passed = 0, failed = 0, skipped = 0;
unsigned legacyLeftDown = 0, legacyLeftUp = 0;
unsigned legacyRightDown = 0, legacyRightUp = 0;
unsigned statusNotifications = 0;
clickshade::Runtime* notificationRuntime = nullptr;
constexpr UINT_PTR kMoveProbeTimer = 81;
struct MoveProbe {
    clickshade::Runtime* runtime = nullptr;
    unsigned stage = 0;
    bool modal = false, clipped = false, held = false, correctRule = false;
};
MoveProbe* moveProbe = nullptr;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    condition ? ++passed : ++failed;
}

void Skip(const char* label) { std::printf("SKIP %s\n", label); ++skipped; }

void Pump(DWORD milliseconds = 220) {
    const ULONGLONG end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(4);
    } while (GetTickCount64() < end);
}

void Mouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD wait = 80) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.dx = dx;
    input.mi.dy = dy;
    if (SendInput(1, &input, sizeof(input)) != 1)
        Check(false, "Windows accepts the test mouse input");
    Pump(wait);
}

struct OverlayInfo { unsigned visible = 0; bool passive = true; };
BOOL CALLBACK CountOverlay(HWND window, LPARAM parameter) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window)) return TRUE;
    wchar_t name[80]{};
    GetClassNameW(window, name, 80);
    if (wcscmp(name, L"ClickShade.VisualOverlay.v1") != 0) return TRUE;
    auto& information = *reinterpret_cast<OverlayInfo*>(parameter);
    ++information.visible;
    constexpr LONG_PTR flags = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                               WS_EX_LAYERED | WS_EX_TOPMOST;
    information.passive &= (GetWindowLongPtrW(window, GWL_EXSTYLE) & flags) == flags;
    return TRUE;
}
OverlayInfo Overlays() {
    OverlayInfo information;
    EnumWindows(CountOverlay, reinterpret_cast<LPARAM>(&information));
    return information;
}

void Describe(clickshade::Runtime& runtime, const char* stage) {
    const auto state = runtime.Snapshot();
    std::printf("STATE %s: ready=%d pressed=%d held=%u fullscreen=%d confined=%d paused=%d overlays=%u\n",
        stage, state.ready, state.pressed, state.heldButtons, state.fullscreen,
        state.cursorConfined, state.pausedByRule, Overlays().visible);
    std::wprintf(L"  %ls | error=%ls | visuals=%ls | audio=%ls\n",
        state.stateText.c_str(), state.lastError.c_str(),
        state.visualError.c_str(), state.audioError.c_str());
}

LRESULT CALLBACK ProbeWindow(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_LBUTTONDOWN: ++legacyLeftDown; break;
    case WM_LBUTTONUP: ++legacyLeftUp; break;
    case WM_RBUTTONDOWN: ++legacyRightDown; break;
    case WM_RBUTTONUP: ++legacyRightUp; break;
    case WM_TIMER:
        if (wParam == kMoveProbeTimer && moveProbe) {
            auto& probe = *moveProbe;
            GUITHREADINFO gui{}; gui.cbSize = sizeof(gui);
            const bool modal = GetGUIThreadInfo(GetCurrentThreadId(), &gui) && (gui.flags & GUI_INMOVESIZE);
            if (probe.stage++ == 0) {
                probe.modal = modal;
                RECT clip{}; GetWindowRect(window, &clip);
                clip.left += 20; clip.top += 20; clip.right -= 20; clip.bottom -= 20;
                probe.clipped = ClipCursor(&clip) != FALSE;
                NotifyWinEvent(EVENT_SYSTEM_MOVESIZESTART, window, OBJID_WINDOW, CHILDID_SELF);
                INPUT movement{}; movement.type = INPUT_MOUSE;
                movement.mi.dwFlags = MOUSEEVENTF_MOVE; movement.mi.dx = 9; movement.mi.dy = 3;
                SendInput(1, &movement, sizeof(movement));
                SetTimer(window, kMoveProbeTimer, 320, nullptr);
            } else {
                const auto state = probe.runtime->Snapshot();
                probe.modal &= modal;
                probe.held = state.pressed && state.heldButtons == 1;
                probe.correctRule = !state.cursorConfined && !state.pausedByRule;
                ClipCursor(nullptr);
                KillTimer(window, kMoveProbeTimer);
                INPUT release{}; release.type = INPUT_MOUSE; release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(1, &release, sizeof(release));
                // A test-only escape also exits a keyboard move loop on a
                // desktop that ignored its mouse-initiated subcommand.
                INPUT escape[2]{};
                for (auto& key : escape) { key.type = INPUT_KEYBOARD; key.ki.wVk = VK_ESCAPE; }
                escape[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, escape, sizeof(INPUT));
            }
            return 0;
        }
        break;
    case WM_APP + 90:
        ++statusNotifications;
        if (notificationRuntime) notificationRuntime->Snapshot();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::wstring OwnName() {
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    std::wstring result(path);
    const auto slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos) result.erase(0, slash + 1);
    CharUpperBuffW(result.data(), static_cast<DWORD>(result.size()));
    return result;
}

struct PointerPreferences {
    std::array<int, 3> acceleration{};
    UINT speed = 0;
    bool valid = false;
    static PointerPreferences Read() {
        PointerPreferences result;
        result.valid = SystemParametersInfoW(SPI_GETMOUSE, 0, result.acceleration.data(), 0) &&
                       SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &result.speed, 0);
        return result;
    }
    bool operator==(const PointerPreferences& other) const {
        return valid && other.valid && acceleration == other.acceleration && speed == other.speed;
    }
};

std::uint64_t PreviewFingerprint(clickshade::Runtime& runtime) {
    HCURSOR cursor = runtime.CopyPreview(false);
    if (!cursor) return 0;
    ICONINFO icon{};
    if (!GetIconInfo(cursor, &icon)) { DestroyCursor(cursor); return 0; }
    std::uint64_t hash = 1469598103934665603ull;
    const auto add = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
    add(icon.xHotspot); add(icon.yHotspot);
    HDC dc = GetDC(nullptr);
    for (HBITMAP bitmap : {icon.hbmMask, icon.hbmColor}) {
        if (!bitmap) { add(0); continue; }
        BITMAP description{}; GetObjectW(bitmap, sizeof(description), &description);
        add(description.bmWidth); add(description.bmHeight);
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(description.bmWidth) * description.bmHeight * 4);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = description.bmWidth; info.bmiHeader.biHeight = -description.bmHeight;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        if (!dc || GetDIBits(dc, bitmap, 0, description.bmHeight, pixels.data(), &info, DIB_RGB_COLORS) != description.bmHeight) hash = 0;
        else for (const auto value : pixels) add(value);
    }
    if (dc) ReleaseDC(nullptr, dc);
    if (icon.hbmMask) DeleteObject(icon.hbmMask);
    if (icon.hbmColor) DeleteObject(icon.hbmColor);
    DestroyCursor(cursor);
    return hash;
}

void AbsoluteMove(int x, int y, DWORD wait = 24) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    Mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
        static_cast<LONG>((static_cast<long long>(x - left) * 65536 + width / 2) / width),
        static_cast<LONG>((static_cast<long long>(y - top) * 65536 + height / 2) / height), wait);
}

void ShakeMouse(HWND window) {
    RECT client{}; GetClientRect(window, &client);
    POINT origin{}; ClientToScreen(window, &origin);
    for (unsigned turn = 0; turn < 8; ++turn)
        AbsoluteMove(origin.x + (turn & 1 ? client.right - 30 : 30), origin.y + 90, 24);
    Pump(240);
}

void TestInputAccessPolicy() {
    using clickshade::InputTokenFacts;
    using clickshade::CanReconcileAsyncButtons;
    const InputTokenFacts medium{SECURITY_MANDATORY_MEDIUM_RID, true, false};
    Check(CanReconcileAsyncButtons(medium, medium) &&
          CanReconcileAsyncButtons(medium, {SECURITY_MANDATORY_LOW_RID, true, false}),
          "equal/lower integrity permits the release-safety check");
    Check(!CanReconcileAsyncButtons(medium, {SECURITY_MANDATORY_HIGH_RID, true, false}) &&
          !CanReconcileAsyncButtons(medium, {}) && !CanReconcileAsyncButtons({}, medium),
          "higher or unknown integrity cannot turn an unreadable asynchronous state into a false release");
    Check(CanReconcileAsyncButtons({0, false, true}, {SECURITY_MANDATORY_SYSTEM_RID, true, false}),
          "an already UIAccess-authorized process may reconcile without requesting new privilege");
    Check(clickshade::ReadInputTokenFacts(GetCurrentProcess()).integrityKnown,
          "current process integrity facts are read through the bounded token query");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || wcscmp(argv[1], L"--run") != 0) {
        std::puts("Run runtime_smoke.exe --run on an interactive test desktop.\n"
                  "The test moves, clips and hides the mouse temporarily, and uses a fullscreen window.");
        return 0;
    }
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    TestInputAccessPolicy();
    const auto initialPreferences = PointerPreferences::Read();
    Check(initialPreferences.valid, "Windows pointer acceleration thresholds and speed are readable");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    POINT initialPointer{};
    GetCursorPos(&initialPointer);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = ProbeWindow;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = L"ClickShade.RuntimeSmoke";
    if (!RegisterClassW(&windowClass)) return 2;
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"ClickShade runtime checks",
        WS_OVERLAPPEDWINDOW, 20, 20, 500, 350, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) return 2;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetCursorPos(150, 150);
    Pump();

    clickshade::Runtime runtime;
    notificationRuntime = &runtime;
    clickshade::Settings options;
    std::wstring error;
    const bool started = runtime.Start(window, WM_APP + 90, options, error);
    Check(started, "runtime starts with the production defaults");
    if (!started) {
        std::wprintf(L"Startup error: %ls\n", error.c_str());
        DestroyWindow(window);
        SetCursorPos(initialPointer.x, initialPointer.y);
        return 1;
    }
    Pump(300);
    Describe(runtime, "initial");
    Check(runtime.Snapshot().ready, "ready snapshot is published");
    Check(options.pauseFullscreen && options.pauseConfinedCursor,
          "fullscreen and confined-pointer protection default on");
    Check(Overlays().visible == 1 && Overlays().passive,
          "idle highlight is visible, topmost, layered, click-through and nonactivating");
    Check(GetForegroundWindow() == window, "highlight does not steal foreground focus");
    const auto originalPreview = PreviewFingerprint(runtime);
    Check(originalPreview != 0, "original cursor preview can be inspected independently of active system handles");
    std::array<POINT, 5> disabledPath{}, enabledPath{};
    const auto samplePath = [&](auto& result) {
        unsigned index = 0;
        for (const POINT point : {POINT{150,150}, POINT{270,160}, POINT{190,205}, POINT{310,185}, POINT{150,150}}) {
            AbsoluteMove(point.x, point.y, 30);
            GetCursorPos(&result[index++]);
        }
    };
    options.enabled = false;
    runtime.UpdateSettings(options); Pump();
    samplePath(disabledPath);
    options.enabled = true;
    runtime.UpdateSettings(options); Pump();
    samplePath(enabledPath);
    bool samePath = true;
    for (unsigned i = 0; i < disabledPath.size(); ++i)
        samePath &= disabledPath[i].x == enabledPath[i].x && disabledPath[i].y == enabledPath[i].y;
    Check(samePath, "enabled effects preserve the same OS-reported absolute pointer path as disabled effects");
    Check(PointerPreferences::Read() == initialPreferences,
          "enabling effects does not change Windows pointer speed or acceleration settings");

    options.highlightEnabled = false;
    options.clickEffectsEnabled = false;
    options.trailEnabled = true;
    options.trailDurationMs = 220;
    runtime.UpdateSettings(options); Pump();
    for (unsigned i = 0; i < 8; ++i) Mouse(MOUSEEVENTF_MOVE, 9, 1, 22);
    Check(Overlays().visible > 0 && Overlays().visible <= 32 && Overlays().passive,
          "production Raw Input wakes a trail when the steady highlight and click effects are disabled");
    Pump(300);
    Check(Overlays().visible == 0, "trail-only runtime expires every stamp after movement ends");
    options.trailEnabled = false;
    options.highlightEnabled = true;
    options.clickEffectsEnabled = true;
    runtime.UpdateSettings(options); Pump();

    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 1,
          "left down activates darkening");
    Mouse(MOUSEEVENTF_RIGHTDOWN);
    Check(runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 3,
          "overlapping left and right buttons are tracked");
    Mouse(MOUSEEVENTF_LEFTUP);
    Check(runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 2,
          "first release leaves the remaining selected button active");
    Mouse(MOUSEEVENTF_RIGHTUP);
    Check(!runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 0,
          "final release clears darkening");
    Check(legacyLeftDown == 1 && legacyLeftUp == 1 && legacyRightDown == 1 && legacyRightUp == 1,
          "foreground app receives all normal left and right mouse messages");

    Mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 240);
    Check(runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 1,
          "default hold mode stays pressed beyond the short-pulse duration");
    Mouse(MOUSEEVENTF_LEFTUP);

    Mouse(MOUSEEVENTF_LEFTDOWN);
    MoveProbe drag; drag.runtime = &runtime;
    moveProbe = &drag;
    SetTimer(window, kMoveProbeTimer, 80, nullptr);
    POINT moveStart{}; GetCursorPos(&moveStart);
    SendMessageW(window, WM_SYSCOMMAND, SC_MOVE | 2, MAKELPARAM(moveStart.x, moveStart.y));
    Pump(500);
    KillTimer(window, kMoveProbeTimer);
    moveProbe = nullptr;
    ClipCursor(nullptr);
    if (wine && !drag.modal) {
        Skip("Wine did not expose GUI_INMOVESIZE for its standard move loop; native drag confinement assertion remains unverified");
    } else {
        Check(drag.modal && drag.clipped, "real standard window move loop temporarily clips the pointer in the test");
        Check(drag.held && drag.correctRule,
              "standard move/resize confinement cannot falsely pause or release a held cursor");
    }
    Mouse(MOUSEEVENTF_LEFTUP);
    Check(!runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 0,
          "move-loop release clears held state normally after the drag");
    SetWindowPos(window, nullptr, 20, 20, 500, 350, SWP_NOZORDER | SWP_NOACTIVATE);
    SetCursorPos(150,150); Pump();

    options.cursorHold = false;
    options.cursorFlashMs = 240;
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 25);
    Mouse(MOUSEEVENTF_LEFTUP, 0, 0, 25);
    Check(runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 0,
          "pulse mode can complete its duration after a quick button release");
    Pump(300);
    Check(!runtime.Snapshot().pressed, "short pulse restores the released appearance at its deadline");
    Mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 320);
    Check(!runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 1,
          "pulse mode expires even when the physical button remains held");
    Mouse(MOUSEEVENTF_LEFTUP);
    options.cursorFlashMs = 300;
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 200);
    Mouse(MOUSEEVENTF_LEFTUP, 0, 0, 4);
    Mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 200);
    Check(runtime.Snapshot().pressed, "a fresh selected press restarts the pulse deadline");
    Mouse(MOUSEEVENTF_LEFTUP, 0, 0, 4);
    options.enabled = false;
    runtime.UpdateSettings(options); Pump(70);
    Check(!runtime.Snapshot().pressed && Overlays().visible == 0,
          "master disable cancels a pending pulse immediately");
    options.enabled = true;
    options.cursorHold = true;
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_MIDDLEDOWN);
    Check(!runtime.Snapshot().pressed, "unselected middle button does not darken");
    Mouse(MOUSEEVENTF_MIDDLEUP);
    options.buttonMask = 4;
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_MIDDLEDOWN);
    Check(runtime.Snapshot().pressed, "selected middle button darkens");
    Mouse(MOUSEEVENTF_MIDDLEUP);

    options.buttonMask = 3;
    options.darkCursorEnabled = false;
    options.shakeStyle = 1;
    options.shakeSensitivity = 200;
    options.shakeMaximumScale = 300;
    options.highlightEnabled = false;
    options.clickEffectsEnabled = false;
    runtime.UpdateSettings(options); Pump(750);
    Check(runtime.Snapshot().ready && PreviewFingerprint(runtime) == originalPreview,
          "shake-only mode with default cursor options keeps the current Windows cursor");
    // The chosen cursor, its scale and look are independent of press changing;
    // reverse mode belongs to press changing and stays inactive without it.
    options.cursorStyle = 2;
    options.cursorScale = 400;
    runtime.UpdateSettings(options); Pump(750);
    const auto chosenPreview = PreviewFingerprint(runtime);
    options.cursorReverse = true;
    runtime.UpdateSettings(options); Pump(750);
    Check(runtime.Snapshot().ready && chosenPreview != originalPreview && PreviewFingerprint(runtime) == chosenPreview,
          "with press changing off the chosen cursor and scale still apply, while reverse mode stays inactive");
    ShakeMouse(window);
    Check(runtime.Snapshot().locatorScale > 1.1f && !runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 0,
          "real Raw Input motion starts a locator without enabling cursor darkening");
    Mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 35);
    Check(runtime.Snapshot().locatorScale > 1.1f && !runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 1,
          "a click during shake-only magnification does not enable the disabled pressed appearance");
    Mouse(MOUSEEVENTF_LEFTUP, 0, 0, 35);
    options.enabled = false;
    runtime.UpdateSettings(options); Pump(80);
    Check(runtime.Snapshot().locatorScale == 1.f && !runtime.Snapshot().pressed,
          "master disable immediately cancels active locator magnification");
    options.enabled = true;
    options.darkCursorEnabled = true;
    options.cursorStyle = 0; options.cursorReverse = false; options.cursorScale = 100;
    runtime.UpdateSettings(options); Pump(650);
    ShakeMouse(window);
    Mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 35);
    Check(runtime.Snapshot().locatorScale > 1.1f && runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 1,
          "locator magnification and the selected held appearance can coexist");
    Mouse(MOUSEEVENTF_LEFTUP, 0, 0, 35);
    Check(!runtime.Snapshot().pressed && runtime.Snapshot().locatorScale > 1.1f,
          "releasing a button restores the locator's released appearance while its hold interval continues");
    Pump(2400);
    Check(runtime.Snapshot().locatorScale == 1.f && runtime.Snapshot().ready,
          "locator growth, hold and return deadlines finish at the normal cursor scale");
    const auto idleUpdates = runtime.Snapshot().updateCount;
    const auto idleNotifications = statusNotifications;
    Pump(250);
    Check(runtime.Snapshot().locatorScale == 1.f && runtime.Snapshot().updateCount == idleUpdates &&
          statusNotifications == idleNotifications,
          "finished locator remains idle without cursor updates or GUI notifications");
    options.shakeStyle = 0; options.shakeSensitivity = 100; options.shakeMaximumScale = 400;
    options.darkCursorEnabled = false;
    options.highlightEnabled = false;
    options.clickEffectsEnabled = true;
    runtime.UpdateSettings(options); Pump(450);
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(!runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 1,
          "darkening can be disabled while tracking button state");
    Check(Overlays().visible > 0, "left click visual works with darkening and highlight disabled");
    const auto beforePalette = runtime.Snapshot();
    const unsigned paletteNotifications = statusNotifications;
    for (unsigned i = 0; i < 256; ++i) {
        options.uiBackground = RGB(i & 127, 22, 30);
        options.uiScalePercent = i & 1 ? 100 : 125;
        runtime.UpdateSettings(options);
    }
    Pump(60);
    Check(Overlays().visible > 0, "GUI-only settings edits do not clear a running click animation");
    Check(runtime.Snapshot().previewRevision == beforePalette.previewRevision &&
          runtime.Snapshot().updateCount == beforePalette.updateCount,
          "GUI-only settings edits do not reconfigure the cursor engine");
    Check(statusNotifications - paletteNotifications <= 3,
          "a burst of GUI-only edits does not create a status-notification storm");
    Mouse(MOUSEEVENTF_LEFTUP); Pump(450);
    Check(Overlays().visible == 0, "completed click animations hide their windows");
    options.leftClickEnabled = false;
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(Overlays().visible == 0, "left click visual can be disabled independently");
    Mouse(MOUSEEVENTF_LEFTUP);
    Mouse(MOUSEEVENTF_RIGHTDOWN);
    Check(Overlays().visible > 0, "right click visual remains enabled independently");
    Mouse(MOUSEEVENTF_RIGHTUP); Pump(450);

    options.darkCursorEnabled = true;
    options.highlightEnabled = true;
    options.leftClickEnabled = true;
    options.enabled = false;
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(!runtime.Snapshot().pressed && Overlays().visible == 0,
          "master disable suppresses darkening, highlight and click visuals");
    Mouse(MOUSEEVENTF_LEFTUP);
    options.enabled = true;
    options.excludedProcesses = L"  \"C:\\sample\\" + OwnName() + L"\" ; unknown.exe ";
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(runtime.Snapshot().pausedByRule && !runtime.Snapshot().pressed && Overlays().visible == 0,
          "case-insensitive executable exclusion suppresses all visible effects");
    Mouse(MOUSEEVENTF_LEFTUP);
    options.excludedProcesses.clear();
    runtime.UpdateSettings(options); Pump();

    options.highlightEnabled = false;
    options.clickEffectsEnabled = false;
    for (unsigned i = 0; i < 256; ++i) {
        options.buttonMask = i & 1 ? 1u : 4u;
        runtime.UpdateSettings(options);
    }
    options.buttonMask = 3;
    runtime.UpdateSettings(options); Pump();
    const unsigned beforeBurstLeft = legacyLeftDown;
    const unsigned beforeBurstUp = legacyLeftUp;
    const unsigned beforeBurstNotifications = statusNotifications;
    const ULONGLONG burstStarted = GetTickCount64();
    std::array<INPUT, 64> burst{};
    for (std::size_t i = 0; i < burst.size(); ++i) {
        burst[i].type = INPUT_MOUSE;
        burst[i].mi.dwFlags = i & 1 ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
    }
    Check(SendInput(static_cast<UINT>(burst.size()), burst.data(), sizeof(INPUT)) == burst.size(),
          "Windows accepts the rapid alternating press/release burst");
    Pump(250);
    Check(legacyLeftDown - beforeBurstLeft == 32 && legacyLeftUp - beforeBurstUp == 32,
          "all burst presses and releases still reach the foreground application");
    Check(!runtime.Snapshot().pressed && runtime.Snapshot().heldButtons == 0,
          "buffered burst finishes with no stale held state");
    Check(statusNotifications - beforeBurstNotifications <=
              (GetTickCount64() - burstStarted) / 40 + 3,
          "GUI notifications are bounded independently of raw button-event count");
    Mouse(MOUSEEVENTF_RIGHTDOWN);
    Check(runtime.Snapshot().pressed, "the final settings snapshot wins after a settings burst");
    Mouse(MOUSEEVENTF_RIGHTUP);
    options.highlightEnabled = true;
    options.clickEffectsEnabled = true;
    runtime.UpdateSettings(options); Pump();

    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
        monitor.rcMonitor.right - monitor.rcMonitor.left,
        monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Describe(runtime, "fullscreen");
    Check(runtime.Snapshot().fullscreen && runtime.Snapshot().pausedByRule &&
          !runtime.Snapshot().pressed && Overlays().visible == 0,
          "fullscreen pauses effects even if an automatic resize event was omitted");
    Mouse(MOUSEEVENTF_LEFTUP);
    options.allowedProcesses = L"  \"C:\\desktop\\" + OwnName() + L"\" ; unknown.exe ";
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(runtime.Snapshot().fullscreen && !runtime.Snapshot().pausedByRule &&
          runtime.Snapshot().pressed && Overlays().visible > 0,
          "a matching desktop-app allowance bypasses fullscreen without changing the global rule");
    Mouse(MOUSEEVENTF_LEFTUP);
    options.excludedProcesses = OwnName();
    runtime.UpdateSettings(options); Pump();
    Check(runtime.Snapshot().pausedByRule && Overlays().visible == 0,
          "an explicit exclusion wins when the same app is also allowed");
    options.excludedProcesses.clear();
    options.allowedProcesses.clear();
    options.pauseFullscreen = false;
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(runtime.Snapshot().pressed && Overlays().visible > 0,
          "fullscreen behavior follows the explicit user override");
    Mouse(MOUSEEVENTF_LEFTUP);
    SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPos(window, HWND_TOP, 20, 20, 500, 350, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    options.pauseFullscreen = true;
    runtime.UpdateSettings(options); Pump(450);
    ShowWindow(window, SW_MAXIMIZE); Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(!runtime.Snapshot().fullscreen && !runtime.Snapshot().pausedByRule && runtime.Snapshot().pressed,
          "an ordinary maximized desktop window is not mistaken for fullscreen");
    Mouse(MOUSEEVENTF_LEFTUP);
    ShowWindow(window, SW_RESTORE); Pump();

    const RECT clip{90, 90, 280, 250};
    Check(ClipCursor(&clip) != FALSE, "test desktop accepts pointer confinement");
    Pump(230);
    Mouse(MOUSEEVENTF_MOVE, 2, 1, 100);
    Check(runtime.Snapshot().cursorConfined && runtime.Snapshot().pausedByRule && Overlays().visible == 0,
          "throttled movement check discovers confinement and hides effects");
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(!runtime.Snapshot().pressed, "confined-pointer rule suppresses darkening on click");
    Mouse(MOUSEEVENTF_LEFTUP);
    options.allowedProcesses = OwnName();
    runtime.UpdateSettings(options); Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(runtime.Snapshot().cursorConfined && !runtime.Snapshot().pausedByRule && runtime.Snapshot().pressed,
          "an explicit desktop-app allowance can bypass temporary pointer confinement");
    Mouse(MOUSEEVENTF_LEFTUP);
    options.allowedProcesses.clear();
    runtime.UpdateSettings(options); Pump();
    Check(runtime.Snapshot().pausedByRule, "removing an allowance restores the confined-pointer rule");
    ClipCursor(nullptr);
    Pump(230);
    Mouse(MOUSEEVENTF_MOVE, 2, 1, 100);
    Check(!runtime.Snapshot().cursorConfined && !runtime.Snapshot().pausedByRule && Overlays().visible > 0,
          "effects resume after confinement is removed");

    options.allowedProcesses = OwnName();
    runtime.UpdateSettings(options); Pump();
    int hiddenCalls = 0;
    do { ++hiddenCalls; } while (ShowCursor(FALSE) >= 0 && hiddenCalls < 30);
    NotifyWinEvent(EVENT_OBJECT_HIDE, nullptr, OBJID_CURSOR, CHILDID_SELF);
    Pump();
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(runtime.Snapshot().pausedByRule && !runtime.Snapshot().pressed && Overlays().visible == 0,
          "an allowed application still pauses every effect when it intentionally hides the pointer");
    Mouse(MOUSEEVENTF_LEFTUP);
    while (hiddenCalls-- > 0) ShowCursor(TRUE);
    NotifyWinEvent(EVENT_OBJECT_SHOW, nullptr, OBJID_CURSOR, CHILDID_SELF);
    Pump();
    const bool resumedByEvent = !runtime.Snapshot().pausedByRule && Overlays().visible > 0;
    if (wine && !resumedByEvent)
        Skip("stationary cursor-show WinEvent delivery is missing in this Wine desktop; input fallback follows");
    else Check(resumedByEvent, "cursor-show events resume effects without requiring pointer movement");
    Pump(230);
    Mouse(MOUSEEVENTF_MOVE, 1, 1, 100);
    Check(!runtime.Snapshot().pausedByRule && Overlays().visible > 0,
          "input fallback resumes effects after a missed cursor-show event");
    options.allowedProcesses.clear();

    options.leftSoundPath = L"C:\\ClickShade_runtime_test_missing.wav";
    options.clickSoundsEnabled = true;
    runtime.UpdateSettings(options); Pump(800);
    Check(!runtime.Snapshot().audioError.empty(), "bad custom audio produces an optional-service diagnostic");
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Check(runtime.Snapshot().ready && runtime.Snapshot().pressed,
          "optional sound failure does not disable the native cursor effect");
    Mouse(MOUSEEVENTF_LEFTUP);
    options.clickSoundsEnabled = false;
    options.leftSoundPath.clear();
    runtime.UpdateSettings(options); Pump();

    if (wine) {
        Skip("native system-cursor mutation/restoration and SPI_SETCURSORS reload: Wine has unsupported/stub APIs");
    } else {
        const auto revision = runtime.Snapshot().previewRevision;
        runtime.RefreshCursors(); Pump(800);
        Check(runtime.Snapshot().ready && runtime.Snapshot().previewRevision > revision,
              "system-scheme reload refreshes previews and keeps the runtime ready");
    }
    Mouse(MOUSEEVENTF_LEFTDOWN);
    runtime.Stop();
    Check(!runtime.Snapshot().ready && !runtime.Snapshot().pressed &&
          runtime.Snapshot().heldButtons == 0 && Overlays().visible == 0,
          "stop while held clears state and destroys visual windows");
    Mouse(MOUSEEVENTF_LEFTUP);
    ClipCursor(nullptr);
    notificationRuntime = nullptr;
    clickshade::Runtime recoverable;
    notificationRuntime = &recoverable;
    options.cursorStyle = 3;
    options.releasedCursorPath = L"C:\\ClickShade_runtime_test_missing.cur";
    const bool profileStarted = recoverable.Start(window, WM_APP + 90, options, error);
    Check(profileStarted && !recoverable.Snapshot().ready && !recoverable.Snapshot().lastError.empty(),
          "missing saved custom cursor leaves a running, editable runtime with an error");
    if (profileStarted) {
        options.cursorStyle = 0;
        options.releasedCursorPath.clear();
        recoverable.UpdateSettings(options); Pump();
        Mouse(MOUSEEVENTF_LEFTDOWN);
        Check(recoverable.Snapshot().ready && recoverable.Snapshot().pressed &&
              recoverable.Snapshot().lastError.empty(),
              "selecting a valid cursor profile recovers without restarting the app");
        Mouse(MOUSEEVENTF_LEFTUP);
    }
    recoverable.Stop();
    Check(PointerPreferences::Read() == initialPreferences,
          "complete effects/shake/drag/start-stop sequence preserves Windows pointer preferences");
    notificationRuntime = nullptr;
    DestroyWindow(window);
    SetCursorPos(initialPointer.x, initialPointer.y);
    std::printf("RESULT %d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
