#include "runtime.h"
#include "cursor_engine.h"
#include "visual_effects.h"
#include "click_audio.h"
#include "input_access.h"
#include "key_display.h"
#include "win_event_listener.h"
#include "shake_locator.h"
#include "frame_pacing.h"

#include <dwmapi.h>
#include <wtsapi32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace clickshade {
namespace {
constexpr wchar_t kInputClass[] = L"ClickShade.InputWindow.v1";
constexpr UINT kSettingsChanged = WM_APP + 41;
constexpr UINT kRefreshCursors = WM_APP + 42;
constexpr UINT kForegroundChanged = WM_APP + 43;
constexpr UINT kCursorChanged = WM_APP + 44;
constexpr UINT kDesktopChanged = WM_APP + 45;
constexpr UINT kOptionalServiceChanged = WM_APP + 46;
constexpr UINT kForegroundGeometryChanged = WM_APP + 47;
constexpr UINT_PTR kReleaseSafetyTimer = 1;
constexpr UINT_PTR kForegroundTimer = 2;
constexpr UINT_PTR kReloadTimer = 3;
constexpr UINT_PTR kStatusTimer = 4;
constexpr UINT_PTR kPulseTimer = 5;
constexpr UINT kReleaseSafetyMs = 200;
constexpr UINT kStatusNotificationMs = 40;
constexpr unsigned kAllButtons = 31;

std::wstring ErrorText(const wchar_t* action, DWORD code = GetLastError()) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result(action);
    result += L" (Windows error " + std::to_wstring(code) + L")";
    if (message) {
        result += L": ";
        result += message;
        LocalFree(message);
        while (!result.empty() && iswspace(result.back())) result.pop_back();
    }
    return result;
}

std::wstring FoldName(std::wstring name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return name;
}

std::vector<std::wstring> ParseExclusions(const std::wstring& text) {
    std::vector<std::wstring> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(L';', begin);
        auto item = text.substr(begin, end == std::wstring::npos ? end : end - begin);
        auto first = item.find_first_not_of(L" \t\r\n\"");
        if (first != std::wstring::npos) {
            auto last = item.find_last_not_of(L" \t\r\n\"");
            item = item.substr(first, last - first + 1);
            const auto slash = item.find_last_of(L"\\/");
            if (slash != std::wstring::npos) item.erase(0, slash + 1);
            if (!item.empty()) result.push_back(FoldName(std::move(item)));
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return result;
}

bool IsInputDesktopActive() {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    wchar_t inputName[256]{}, threadName[256]{};
    DWORD needed = 0;
    const BOOL gotInput = GetUserObjectInformationW(input, UOI_NAME, inputName,
                                                   sizeof(inputName), &needed);
    const BOOL gotThread = GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),
                            UOI_NAME, threadName, sizeof(threadName), &needed);
    CloseDesktop(input);
    return gotInput && gotThread && _wcsicmp(inputName, threadName) == 0;
}

bool IsShellWindowLike(HWND window) {
    if (!window || window == GetDesktopWindow() || window == GetShellWindow()) return true;
    wchar_t className[80]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return _wcsicmp(className, L"Progman") == 0 || _wcsicmp(className, L"WorkerW") == 0 ||
        _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

bool IsFullscreenWindow(HWND window, bool shellWindow) {
    if (!window || shellWindow || !IsWindowVisible(window) || IsIconic(window)) return false;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    // A maximized desktop window can occupy the entire monitor when the
    // taskbar auto-hides or an application custom-draws its title bar.
    // Keep ordinary resizable/captioned maximized windows usable.
    if (IsZoomed(window) && (style & (WS_CAPTION | WS_THICKFRAME))) return false;
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return false;
    POINT first{client.left, client.top}, last{client.right, client.bottom};
    if (!ClientToScreen(window, &first) || !ClientToScreen(window, &last)) return false;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const auto closeEnough = [](LONG a, LONG b) { return a >= b - 2 && a <= b + 2; };
    if (!closeEnough(first.x, monitor.rcMonitor.left) || !closeEnough(first.y, monitor.rcMonitor.top) ||
        !closeEnough(last.x, monitor.rcMonitor.right) || !closeEnough(last.y, monitor.rcMonitor.bottom))
        return false;
    // Most desktop clicks are in ordinary windows. Only query the compositor
    // for a fullscreen candidate; its cloaking state cannot change that result.
    DWORD cloaked = 0;
    return FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || !cloaked;
}

bool IsCursorConfined(bool shellWindow) {
    if (shellWindow) return false;
    GUITHREADINFO gui{};
    gui.cbSize = sizeof(gui);
    // Windows may temporarily constrain the pointer during its ordinary
    // desktop move/size loop. That is not a game's pointer-lock signal.
    if (GetGUIThreadInfo(0, &gui) && (gui.flags & GUI_INMOVESIZE)) return false;
    RECT clip{};
    if (!GetClipCursor(&clip)) return false;
    const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG right = left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return clip.left > left + 2 || clip.top > top + 2 ||
           clip.right < right - 2 || clip.bottom < bottom - 2;
}

std::wstring ProcessName(DWORD processId, InputTokenFacts& tokenFacts) {
    tokenFacts = {};
    if (!processId) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return L"(process unavailable)";
    tokenFacts = ReadInputTokenFacts(process);
    std::array<wchar_t, 32768> path;
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL success = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!success) return L"(process unavailable)";
    std::wstring name(path.data(), length);
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    return name;
}

unsigned AsyncHeldButtons() {
    constexpr int keys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    unsigned result = 0;
    for (unsigned i = 0; i < std::size(keys); ++i)
        if (GetAsyncKeyState(keys[i]) & 0x8000) result |= 1u << i;
    return result;
}

bool SameVisualSettings(const Settings& a, const Settings& b) {
    return a.enabled == b.enabled && a.highlightEnabled == b.highlightEnabled &&
        a.highlightStyle == b.highlightStyle && a.highlightRadius == b.highlightRadius &&
        a.highlightOpacity == b.highlightOpacity && a.highlightColor == b.highlightColor &&
        a.highlightThickness == b.highlightThickness && a.visualRefreshRate == b.visualRefreshRate &&
        a.highlightBreathing == b.highlightBreathing && a.highlightIdleHideMs == b.highlightIdleHideMs &&
        a.spotlightDim == b.spotlightDim && a.spotlightRadius == b.spotlightRadius &&
        a.trailEnabled == b.trailEnabled && a.trailStyle == b.trailStyle &&
        a.trailRadius == b.trailRadius && a.trailOpacity == b.trailOpacity &&
        a.trailDurationMs == b.trailDurationMs && a.trailColor == b.trailColor &&
        a.clickEffectsEnabled == b.clickEffectsEnabled && a.leftClickEnabled == b.leftClickEnabled &&
        a.rightClickEnabled == b.rightClickEnabled && a.middleClickEnabled == b.middleClickEnabled &&
        a.clickStyle == b.clickStyle &&
        a.clickRadius == b.clickRadius && a.clickDurationMs == b.clickDurationMs &&
        a.clickOpacity == b.clickOpacity && a.leftClickColor == b.leftClickColor &&
        a.rightClickColor == b.rightClickColor && a.middleClickColor == b.middleClickColor &&
        a.clickThickness == b.clickThickness && a.clickEasing == b.clickEasing &&
        a.captureVisible == b.captureVisible &&
        a.magnifierEnabled == b.magnifierEnabled && a.magnifierZoom == b.magnifierZoom &&
        a.magnifierSize == b.magnifierSize && a.magnifierShape == b.magnifierShape &&
        a.magnifierPosition == b.magnifierPosition && a.magnifierBorderColor == b.magnifierBorderColor &&
        a.magnifierFrame == b.magnifierFrame &&
        a.keyDisplayEnabled == b.keyDisplayEnabled && a.keyDisplayMouse == b.keyDisplayMouse &&
        a.keyDisplayPosition == b.keyDisplayPosition && a.keyDisplaySize == b.keyDisplaySize &&
        a.keyDisplayDurationMs == b.keyDisplayDurationMs && a.keyDisplayOpacity == b.keyDisplayOpacity &&
        a.keyDisplayColor == b.keyDisplayColor;
}

bool SameAudioSettings(const Settings& a, const Settings& b) {
    return a.clickSoundsEnabled == b.clickSoundsEnabled && a.leftSoundPath == b.leftSoundPath &&
        a.rightSoundPath == b.rightSoundPath && a.middleSoundPath == b.middleSoundPath &&
        a.soundVolume == b.soundVolume && a.keyboardSoundsEnabled == b.keyboardSoundsEnabled &&
        a.keyboardSoundPath == b.keyboardSoundPath && a.keyboardVolume == b.keyboardVolume;
}

unsigned KeyCategoryOf(USHORT virtualKey) noexcept {
    switch (virtualKey) {
    case VK_SPACE: return kKeySpace;
    case VK_RETURN: return kKeyEnter;
    case VK_BACK: return kKeyBackspace;
    default: return kKeyGeneric;
    }
}

// Any cursor option besides the press darkening: the cursor engine then
// replaces the cursors even while "Change cursor on press" is off.
bool CursorCustomized(const Settings& s) noexcept {
    return s.cursorStyle != 0 || s.cursorScale != 100 || s.cursorOpacity < 100 || s.cursorShadow || s.cursorColors;
}

bool SameCursorAppearance(const Settings& a, const Settings& b) {
    return a.cursorStyle == b.cursorStyle && a.cursorReverse == b.cursorReverse &&
        a.cursorOpacity == b.cursorOpacity && a.cursorShadow == b.cursorShadow && a.cursorColors == b.cursorColors &&
        a.cursorFillColor == b.cursorFillColor && a.cursorOutlineColor == b.cursorOutlineColor &&
        a.cursorScale == b.cursorScale && a.releasedCursorPath == b.releasedCursorPath &&
        a.pressedCursorPath == b.pressedCursorPath &&
        a.darkCursorEnabled == b.darkCursorEnabled &&
        (a.shakeStyle != 0) == (b.shakeStyle != 0) &&
        a.shakeMaximumScale == b.shakeMaximumScale;
}

CursorAppearance AppearanceFrom(const Settings& settings) {
    CursorAppearance appearance;
    if (settings.darkCursorEnabled || CursorCustomized(settings)) {
        appearance.style = settings.cursorStyle;
        appearance.reverse = settings.darkCursorEnabled && settings.cursorReverse;
        appearance.scale = settings.cursorScale;
        appearance.releasedPath = settings.releasedCursorPath;
        appearance.pressedPath = settings.pressedCursorPath;
        appearance.opacity = settings.cursorOpacity;
        appearance.shadow = settings.cursorShadow;
        appearance.tint = settings.cursorColors;
        appearance.fillColor = settings.cursorFillColor;
        appearance.outlineColor = settings.cursorOutlineColor;
    }
    appearance.maximumMagnification = settings.shakeStyle
        ? std::clamp(settings.shakeMaximumScale, 150, 400) / 100.f : 1.f;
    return appearance;
}

bool KnownUnrelatedSetting(WPARAM action, LPARAM section) {
    switch (action) {
    case SPI_SETBEEP:
    case SPI_SETDESKWALLPAPER:
    case SPI_SETDESKPATTERN:
    case SPI_SETMOUSE:
    case SPI_SETMOUSESPEED:
    case SPI_SETDOUBLECLICKTIME:
    case SPI_SETDOUBLECLKWIDTH:
    case SPI_SETDOUBLECLKHEIGHT:
    case SPI_SETKEYBOARDSPEED:
    case SPI_SETKEYBOARDDELAY:
        return true;
    default:
        // Unknown broadcasts still refresh the cursor cache, including the
        // accessibility/control-panel paths used by different Windows builds.
        return !action && section &&
            _wcsicmp(reinterpret_cast<const wchar_t*>(section), L"Environment") == 0;
    }
}
} // namespace

struct Runtime::Impl {
    std::thread worker;
    std::atomic<HWND> inputWindow{nullptr};
    std::atomic<DWORD> threadId{0};
    std::atomic<bool> stopRequested{false};
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;

    mutable std::mutex snapshotMutex;
    RuntimeStatus snapshot;
    bool statusNotificationQueued = false; // Protected by snapshotMutex.
    HCURSOR previewNormal = nullptr;
    HCURSOR previewDark = nullptr;
    std::mutex settingsMutex;
    Settings pendingSettings;
    bool settingsQueued = false; // Protected by settingsMutex.
    std::mutex startupMutex;
    std::condition_variable startupCondition;
    bool startupComplete = false;
    bool startupSuccess = false;
    std::wstring startupError;

    // Everything below this point is owned exclusively by the input thread.
    CursorEngine engine;
    VisualEffects visuals;
    ClickAudio audio;
    Settings settings;
    RuntimeStatus state;
    std::vector<std::wstring> exclusions;
    std::vector<std::wstring> allowances;
    std::unordered_map<HANDLE, unsigned> devices;
    WinEventListener events;
    bool rawRegistered = false;
    bool keyboardRegistered = false;
    // Which keys are currently down, so autorepeat makes do not retrigger a
    // sound or a label. Only this bit set and the chord bookkeeping below
    // exist; no key identity is kept once an event has been handled.
    std::array<bool, 256> keyDown{};
    bool rightAltDown = false;
    bool modifierChordUsed = false; // A non-modifier key was pressed while modifiers were held.
    unsigned chordModifiers = 0; // Every modifier held at some point during the current chord.
    bool keyServicesAllowed = false;
    bool sessionRegistered = false;
    bool inputDesktopActive = true;
    bool suspended = false;
    bool excluded = false;
    bool alwaysAllowed = false;
    std::atomic<bool> foregroundQueued{false};
    std::atomic<bool> foregroundGeometryQueued{false};
    std::atomic<bool> cursorQueued{false};
    std::atomic<bool> desktopQueued{false};
    std::atomic<HWND> foregroundEventWindow{nullptr};
    bool reloadPending = false;
    bool reloading = false;
    bool releaseTimerArmed = false;
    bool bufferingAvailable = true;
    bool inputBatchActive = false;
    bool statusDirty = false;
    bool pointerActivityPending = false;
    bool statusTimerArmed = false;
    bool pulseTimerArmed = false;
    bool engineActive = false;
    bool appearanceValid = false;
    bool servicesInitialized = false;
    bool servicesAllowed = false;
    bool cursorVisible = true;
    bool foregroundIsShell = true;
    bool foregroundTimerArmed = false;
    bool asyncButtonsReadable = false;
    InputTokenFacts ownTokenFacts;
    ULONGLONG lastInputRuleCheck = 0;
    ULONGLONG lastStatusNotification = 0;
    ULONGLONG pulseUntil = 0;
    HWND foregroundWindow = nullptr;
    DWORD foregroundProcessId = 0;
    LARGE_INTEGER frequency{};
    ShakeLocator shake;
    AdaptiveFramePacer shakePacer;
    HANDLE shakeTimer = nullptr;
    bool shakeTimerArmed = false;
    double shakeDeadline = std::numeric_limits<double>::infinity();
    double lastShakeFrame = 0.0;
    std::atomic<float> locatorScale{1.f};
    HMONITOR shakeMonitor = nullptr;
    unsigned shakeMonitorHz = 60;
    unsigned shakeDpi = 96;
    HMODULE shakeScaleLibrary = nullptr;
    using MonitorDpiFunction = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
    MonitorDpiFunction monitorDpi = nullptr;

    ~Impl() {
        if (previewNormal) DestroyCursor(previewNormal);
        if (previewDark) DestroyCursor(previewDark);
    }

    double Seconds() const noexcept {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return frequency.QuadPart > 0 ? static_cast<double>(value.QuadPart) /
            static_cast<double>(frequency.QuadPart) : GetTickCount64() / 1000.0;
    }

    void CancelShake() noexcept {
        shake.Reset();
        locatorScale.store(1.f, std::memory_order_relaxed);
        if (shakeTimerArmed && shakeTimer) CancelWaitableTimer(shakeTimer);
        shakeTimerArmed = false;
        shakeDeadline = std::numeric_limits<double>::infinity();
        lastShakeFrame = 0.0;
    }

    void ScheduleShake(double sampledAt, double completedAt) {
        // Keep the deadline derived from the scale actually applied. If the
        // cursor API crosses a transition boundary, run its terminal frame
        // immediately instead of losing the final1x restoration.
        const double deadline = shake.NextWake(sampledAt, 1.0 / shakePacer.Rate());
        if (!std::isfinite(deadline)) {
            if (shakeTimerArmed && shakeTimer) CancelWaitableTimer(shakeTimer);
            shakeTimerArmed = false;
            shakeDeadline = deadline;
            return;
        }
        if (shakeTimerArmed && std::abs(deadline - shakeDeadline) < 0.000001) return;
        if (!shakeTimer) {
            constexpr DWORD highResolution = 0x00000002;
            shakeTimer = CreateWaitableTimerExW(nullptr, nullptr, highResolution,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (!shakeTimer) shakeTimer = CreateWaitableTimerExW(nullptr, nullptr, 0,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
        if (!shakeTimer) { Fail(ErrorText(L"Could not create the cursor locator timer")); return; }
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(std::max(1.0, (deadline - completedAt) * 10000000.0));
        if (!SetWaitableTimerEx(shakeTimer, &due, 0, nullptr, nullptr, nullptr, 0)) {
            Fail(ErrorText(L"Could not schedule the cursor locator")); return;
        }
        shakeDeadline = deadline;
        shakeTimerArmed = true;
    }

    void TickShake() {
        const double now = Seconds();
        const bool allowed = MayApply() && settings.shakeStyle != 0;
        if (!allowed) CancelShake();
        const float scale = allowed ? static_cast<float>(shake.Scale(now)) : 1.f;
        std::wstring error;
        if (!engine.SetMagnification(scale, error)) { Fail(error); return; }
        locatorScale.store(scale, std::memory_order_relaxed);
        const double finished = Seconds();
        shakePacer.Observe(finished - now, lastShakeFrame > 0.0 ? now - lastShakeFrame : 0.0,
                           allowed && shake.Active(now));
        lastShakeFrame = now;
        ScheduleShake(now, finished);
    }

    void ObserveShakeMotion() {
        if (!settings.shakeStyle || !MayApply()) return;
        POINT position{};
        if (!GetCursorPos(&position)) { shake.ClearTrace(); return; }
        const HMONITOR monitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
        if (monitor != shakeMonitor) {
            shakeMonitor = monitor;
            UINT dpi = GetDpiForSystem(), other = dpi;
            if (monitorDpi && FAILED(monitorDpi(monitor, 0, &dpi, &other))) dpi = 96;
            dpi = std::clamp(dpi, 96u, 480u);
            if (dpi != shakeDpi) {
                CancelShake();
                std::wstring error;
                if (!engine.SetMagnification(1.f, error)) { Fail(error); return; }
            }
            else shake.ClearTrace();
            shakeDpi = dpi;
            MONITORINFOEXW info{}; info.cbSize = sizeof(info);
            DEVMODEW mode{}; mode.dmSize = sizeof(mode);
            shakeMonitorHz = 60;
            if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)) &&
                EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) &&
                (mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency > 1)
                shakeMonitorHz = std::clamp(static_cast<unsigned>(mode.dmDisplayFrequency), 30u, 360u);
            shakePacer.Configure(shakeMonitorHz, settings.visualRefreshRate);
        }
        const double now = Seconds();
        const bool eligible = state.heldButtons == 0 && !state.cursorConfined;
        const bool triggered = shake.Observe(position.x * 96.0 / shakeDpi,
            position.y * 96.0 / shakeDpi, now, eligible);
        if (triggered) TickShake();
    }

    void NotifyStatus() {
        if (!notifyWindow || !notifyMessage || !IsWindowVisible(notifyWindow) || IsIconic(notifyWindow))
            return;
        {
            std::lock_guard<std::mutex> lock(snapshotMutex);
            if (statusNotificationQueued) return;
        }
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG elapsed = now - lastStatusNotification;
        if (elapsed < kStatusNotificationMs && inputWindow.load()) {
            if (!statusTimerArmed)
                statusTimerArmed = SetTimer(inputWindow.load(), kStatusTimer,
                    static_cast<UINT>(kStatusNotificationMs - elapsed), nullptr) != 0;
            if (statusTimerArmed) return;
        }
        {
            std::lock_guard<std::mutex> lock(snapshotMutex);
            if (statusNotificationQueued) return;
            statusNotificationQueued = true;
        }
        if (PostMessageW(notifyWindow, notifyMessage, 0, 0)) lastStatusNotification = now;
        else {
            std::lock_guard<std::mutex> lock(snapshotMutex);
            statusNotificationQueued = false;
        }
    }

    void Publish(bool updateText = true) {
        // All button transitions still reach the engine immediately. Only
        // diagnostic copying/GUI notifications wait until the raw batch ends.
        if (inputBatchActive) { statusDirty = true; return; }
        statusDirty = false;
        if (updateText) UpdateStateText();
        {
            std::lock_guard<std::mutex> lock(snapshotMutex);
            snapshot = state;
        }
        NotifyStatus();
    }

    void ResetKeyState() noexcept {
        keyDown.fill(false);
        rightAltDown = false;
        modifierChordUsed = false;
        chordModifiers = 0;
    }

    bool KeyboardWanted() const noexcept {
        return settings.keyboardSoundsEnabled || settings.keyDisplayEnabled;
    }

    // Keyboard observation exists only while keyboard sounds or the key
    // display are enabled. With both switches off the process never receives
    // keyboard input at all.
    void RegisterKeyboard(bool wanted) {
        if (wanted == keyboardRegistered) return;
        const HWND window = inputWindow.load();
        if (!window) return;
        RAWINPUTDEVICE keyboard{0x01, 0x06, static_cast<DWORD>(wanted ? (RIDEV_INPUTSINK | RIDEV_DEVNOTIFY) : RIDEV_REMOVE),
                                wanted ? window : nullptr};
        if (RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard))) keyboardRegistered = wanted;
        else if (wanted) {
            state.audioError = ErrorText(L"Could not subscribe to keyboard input for keyboard sounds and the key display");
            Publish();
        }
        ResetKeyState();
    }

    unsigned CurrentModifiers() const noexcept {
        unsigned modifiers = 0;
        if (keyDown[VK_CONTROL]) modifiers |= kModControl;
        if (keyDown[VK_SHIFT]) modifiers |= kModShift;
        if (keyDown[VK_MENU]) modifiers |= kModAlt;
        if (keyDown[VK_LWIN] || keyDown[VK_RWIN]) modifiers |= kModWin;
        // AltGr arrives as right Alt plus a synthetic left Ctrl.
        if (rightAltDown && (modifiers & kModControl)) modifiers = (modifiers & ~(kModControl | kModAlt)) | kModAltGr;
        // Only the case of typed letters on the key display depends on it.
        if (GetKeyState(VK_CAPITAL) & 1) modifiers |= kModCapsLock;
        return modifiers;
    }

    bool AnyModifierDown() const noexcept {
        return keyDown[VK_CONTROL] || keyDown[VK_SHIFT] || keyDown[VK_MENU] || keyDown[VK_LWIN] || keyDown[VK_RWIN];
    }

    void ShowKey(unsigned virtualKey, unsigned scanCode, bool extended, unsigned modifiers) {
        if (!servicesInitialized || !settings.keyDisplayEnabled || !MayApply(true)) return;
        if (!KeyDisplayWanted(settings.keyDisplayMode, virtualKey, modifiers)) return;
        visuals.Key(virtualKey, scanCode, extended, modifiers);
    }

    void HandleKeyboard(const RAWINPUT& input) {
        if (input.header.dwType != RIM_TYPEKEYBOARD || suspended || state.sessionLocked) return;
        const auto& key = input.data.keyboard;
        if (key.VKey == 0 || key.VKey >= 255) return;
        const bool released = (key.Flags & RI_KEY_BREAK) != 0;
        const bool extended = (key.Flags & RI_KEY_E0) != 0;
        const bool wasDown = keyDown[key.VKey];
        keyDown[key.VKey] = !released;
        if (key.VKey == VK_MENU && extended) rightAltDown = !released;
        const bool modifier = IsModifierKey(key.VKey);
        if (released) {
            // A modifier tapped on its own is a shortcut too (Win opens Start,
            // Alt focuses menus). It shows once every modifier is up again.
            if (modifier && wasDown && !AnyModifierDown()) {
                if (!modifierChordUsed && chordModifiers) {
                    if (chordModifiers & kModAltGr) ShowKey(VK_RMENU, key.MakeCode, true, kModAltGr);
                    else ShowKey(key.VKey, key.MakeCode, extended, chordModifiers & ~ModifierBit(key.VKey));
                }
                modifierChordUsed = false;
                chordModifiers = 0;
            }
            return;
        }
        if (wasDown) return; // Autorepeat stays silent and unlabelled.
        if (modifier) {
            chordModifiers |= ModifierBit(key.VKey);
            if (rightAltDown && keyDown[VK_CONTROL]) chordModifiers |= kModAltGr;
            return;
        }
        if (AnyModifierDown()) modifierChordUsed = true;
        if (servicesInitialized && settings.keyboardSoundsEnabled && MayApply())
            audio.Key(KeyCategoryOf(key.VKey));
        ShowKey(key.VKey, key.MakeCode, extended, CurrentModifiers());
    }

    void UpdateStateText() {
        state.pressed = engine.IsPressed();
        state.pausedByRule = excluded || (!alwaysAllowed &&
                            ((settings.pauseFullscreen && state.fullscreen) ||
                             (settings.pauseConfinedCursor && state.cursorConfined))) || !cursorVisible;
        if (!state.ready) state.stateText = state.lastError.empty() ? L"Cursor engine stopped." : L"Paused after an error. Use Reload cursors to retry.";
        else if (!settings.enabled) state.stateText = L"Disabled. Your normal cursors are restored.";
        else if (state.sessionLocked) state.stateText = L"Paused while the Windows session is locked or disconnected.";
        else if (suspended) state.stateText = L"Paused while Windows is suspended.";
        else if (!inputDesktopActive) state.stateText = L"Paused while this desktop is inactive.";
        else if (excluded) state.stateText = L"Paused for this excluded app.";
        else if (!alwaysAllowed && settings.pauseFullscreen && state.fullscreen) state.stateText = L"Paused by your fullscreen rule.";
        else if (!alwaysAllowed && settings.pauseConfinedCursor && state.cursorConfined) state.stateText = L"Paused because this app confines the pointer.";
        else if (!cursorVisible) state.stateText = L"Paused while this app hides the pointer.";
        else if (!settings.darkCursorEnabled) state.stateText = L"Ready. Cursor darkening is switched off.";
        else if (!settings.buttonMask) state.stateText = L"Ready. Select a mouse button to use cursor darkening.";
        else if (state.pressed && !settings.cursorHold) state.stateText = L"The short-click cursor pulse is active.";
        else if (state.pressed) state.stateText = L"Button held. Supported Windows cursors use the pressed appearance.";
        else if (!settings.cursorHold) state.stateText = L"Ready. Press a selected mouse button for a short cursor pulse.";
        else state.stateText = L"Ready. Hold a selected mouse button to use the pressed cursor.";
    }

    void RecordUpdate(const LARGE_INTEGER& begin) {
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        state.lastUpdateMicroseconds = frequency.QuadPart > 0 ?
            static_cast<double>(end.QuadPart - begin.QuadPart) * 1000000.0 /
            static_cast<double>(frequency.QuadPart) : 0.0;
        ++state.updateCount;
    }

    void SetReleaseTimer() {
        HWND window = inputWindow.load();
        const bool needed = state.heldButtons != 0 && asyncButtonsReadable;
        if (needed && !releaseTimerArmed) {
            releaseTimerArmed = SetTimer(window, kReleaseSafetyTimer, kReleaseSafetyMs, nullptr) != 0;
            if (!releaseTimerArmed) Fail(ErrorText(L"Could not start the release safety timer"));
        } else if (!needed && releaseTimerArmed) {
            KillTimer(window, kReleaseSafetyTimer);
            releaseTimerArmed = false;
        }
    }

    void Fail(const std::wstring& error) {
        CancelShake();
        if (servicesInitialized) {
            visuals.SetAllowed(false);
            audio.SetAllowed(false);
            servicesAllowed = false;
        }
        std::wstring restorationError;
        engine.Restore(restorationError);
        engineActive = false;
        CancelPulse();
        devices.clear();
        state.heldButtons = 0;
        if (releaseTimerArmed) KillTimer(inputWindow.load(), kReleaseSafetyTimer);
        releaseTimerArmed = false;
        state.ready = false;
        state.pressed = false;
        state.lastError = error.empty() ? L"The cursor engine reported an error." : error;
        if (!restorationError.empty()) state.lastError += L" Restore: " + restorationError;
        Publish();
    }

    bool RestoreAndClear() {
        CancelShake();
        CancelPulse();
        devices.clear();
        state.heldButtons = 0;
        SetReleaseTimer();
        std::wstring error;
        const bool wasActive = engineActive || engine.IsPressed();
        LARGE_INTEGER begin{};
        if (wasActive) QueryPerformanceCounter(&begin);
        if (!engine.Restore(error)) { Fail(error); return false; }
        engineActive = false;
        if (wasActive) RecordUpdate(begin);
        state.pressed = false;
        return true;
    }

    // ignoreHiddenPointer: the key display keeps working while an application
    // hides the pointer, which editors do while you type.
    bool MayApply(bool ignoreHiddenPointer = false) const {
        return state.ready && settings.enabled && !state.sessionLocked && !suspended &&
               inputDesktopActive && (cursorVisible || ignoreHiddenPointer) && !excluded &&
               (alwaysAllowed || (!(settings.pauseFullscreen && state.fullscreen) &&
                                  !(settings.pauseConfinedCursor && state.cursorConfined)));
    }

    void CancelPulse() {
        pulseUntil = 0;
        if (pulseTimerArmed) KillTimer(inputWindow.load(), kPulseTimer);
        pulseTimerArmed = false;
    }

    bool StartPulse() {
        const UINT duration = static_cast<UINT>(std::clamp(settings.cursorFlashMs, 50, 500));
        pulseUntil = GetTickCount64() + duration;
        pulseTimerArmed = SetTimer(inputWindow.load(), kPulseTimer, duration, nullptr) != 0;
        if (!pulseTimerArmed) {
            pulseUntil = 0;
            Fail(ErrorText(L"Could not schedule the cursor pulse release"));
            return false;
        }
        return true;
    }

    void UpdateEffect(bool refreshOverlay = false, unsigned clicks = 0, unsigned cursorTriggers = 0) {
        const bool allowed = MayApply();
        const bool keysAllowed = MayApply(true);
        const bool active = allowed && (settings.darkCursorEnabled || settings.shakeStyle != 0 || CursorCustomized(settings));
        if (!allowed || !settings.shakeStyle) CancelShake();
        if (!allowed || !settings.darkCursorEnabled || settings.cursorHold) CancelPulse();
        else if (cursorTriggers && !StartPulse()) return;
        if (servicesInitialized && (allowed != servicesAllowed || keysAllowed != keyServicesAllowed || refreshOverlay)) {
            visuals.SetAllowed(allowed, keysAllowed);
            // Reasserting overlay z-order does not require waking an idle
            // audio worker when its permission is already unchanged.
            if (allowed != servicesAllowed) audio.SetAllowed(allowed);
            servicesAllowed = allowed;
            keyServicesAllowed = keysAllowed;
        }
        // Dispatch independent feedback before cursor API work and diagnostic
        // copying. Workers sample the current pointer, never replay raw deltas.
        if (clicks && servicesInitialized && allowed) {
            if (settings.clickEffectsEnabled) visuals.Click(clicks & 7u);
            if (settings.clickSoundsEnabled) audio.Click(clicks & 7u);
        }
        if (engineActive != active) {
            std::wstring error;
            LARGE_INTEGER begin{};
            QueryPerformanceCounter(&begin);
            if (!engine.SetActive(active, error)) { Fail(error); return; }
            engineActive = active;
            RecordUpdate(begin);
        }
        const bool shouldPress = active && settings.darkCursorEnabled && (settings.cursorHold
            ? (state.heldButtons & settings.buttonMask) != 0
            : pulseUntil != 0 && GetTickCount64() < pulseUntil);
        if (engine.IsPressed() != shouldPress) {
            std::wstring error;
            LARGE_INTEGER begin{};
            QueryPerformanceCounter(&begin);
            if (!engine.SetPressed(shouldPress, error)) { Fail(error); return; }
            RecordUpdate(begin);
        }
        Publish();
    }

    void RefreshPreviews() {
        HCURSOR normal = engine.CopyPreview(false);
        HCURSOR dark = engine.CopyPreview(true);
        {
            std::lock_guard<std::mutex> lock(snapshotMutex);
            std::swap(normal, previewNormal);
            std::swap(dark, previewDark);
            ++state.previewRevision;
        }
        if (normal) DestroyCursor(normal);
        if (dark) DestroyCursor(dark);
    }

    void ReadSettings(bool apply = true) {
        Settings next;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            next = pendingSettings;
            settingsQueued = false;
        }
        next.buttonMask &= kAllButtons;
        const bool exclusionsChanged = next.excludedProcesses != settings.excludedProcesses;
        const bool allowancesChanged = next.allowedProcesses != settings.allowedProcesses;
        const bool rulesChanged = exclusionsChanged || allowancesChanged || next.pauseFullscreen != settings.pauseFullscreen ||
                                  next.pauseConfinedCursor != settings.pauseConfinedCursor;
        const bool appearanceChanged = !SameCursorAppearance(settings, next);
        const bool shakeChanged = next.shakeStyle != settings.shakeStyle ||
            next.shakeSensitivity != settings.shakeSensitivity ||
            next.shakeMaximumScale != settings.shakeMaximumScale;
        const bool pulseChanged = next.cursorHold != settings.cursorHold ||
                                 next.cursorFlashMs != settings.cursorFlashMs ||
                                 next.buttonMask != settings.buttonMask;
        const bool policyChanged = next.enabled != settings.enabled ||
            next.darkCursorEnabled != settings.darkCursorEnabled || pulseChanged || shakeChanged ||
            CursorCustomized(next) != CursorCustomized(settings);
        const bool enablingEffect = (!settings.enabled && next.enabled) ||
            (!settings.darkCursorEnabled && next.darkCursorEnabled) ||
            (!CursorCustomized(settings) && CursorCustomized(next)) ||
            (!settings.highlightEnabled && next.highlightEnabled) ||
            (!settings.trailEnabled && next.trailEnabled) ||
            (!settings.clickEffectsEnabled && next.clickEffectsEnabled) ||
            (!settings.clickSoundsEnabled && next.clickSoundsEnabled) ||
            (!settings.magnifierEnabled && next.magnifierEnabled) ||
            (!settings.keyDisplayEnabled && next.keyDisplayEnabled) ||
            (!settings.shakeStyle && next.shakeStyle);
        const bool visualsChanged = !SameVisualSettings(settings, next);
        const bool audioChanged = !SameAudioSettings(settings, next);
        settings = std::move(next);
        if (shakeChanged || !apply) {
            CancelShake();
            shake.Configure(settings.shakeStyle, settings.shakeSensitivity, settings.shakeMaximumScale);
            if (apply && !appearanceChanged) {
                std::wstring error;
                if (!engine.SetMagnification(1.f, error)) { Fail(error); return; }
            }
        }
        shakePacer.Configure(shakeMonitorHz, settings.visualRefreshRate);
        if (exclusionsChanged) {
            exclusions = ParseExclusions(settings.excludedProcesses);
            const std::wstring name = FoldName(state.foregroundProcess);
            excluded = std::find(exclusions.begin(), exclusions.end(), name) != exclusions.end();
        }
        if (allowancesChanged) {
            allowances = ParseExclusions(settings.allowedProcesses);
            const std::wstring name = FoldName(state.foregroundProcess);
            alwaysAllowed = std::find(allowances.begin(), allowances.end(), name) != allowances.end();
        }
        if (servicesInitialized) {
            if (visualsChanged) visuals.UpdateSettings(settings);
            if (audioChanged) audio.UpdateSettings(settings);
        }
        if (apply) RegisterKeyboard(KeyboardWanted());
        if (!apply) return;
        if (pulseChanged) CancelPulse();
        if (appearanceChanged) {
            CancelShake();
            CancelPulse();
            std::wstring error;
            appearanceValid = false;
            if (!engine.Configure(AppearanceFrom(settings), error)) { Fail(error); return; }
            appearanceValid = true;
            engineActive = false;
            state.ready = true;
            state.lastError.clear();
            state.cursorCount = engine.CursorCount();
            RefreshPreviews();
        }
        if (rulesChanged || enablingEffect || appearanceChanged) UpdateForeground();
        else if (policyChanged) UpdateEffect();
        // UI palette/scaling/startup edits do not reload graphics, rescale
        // sound samples, inspect processes, or disturb an active cursor press.
    }

    bool RefreshCursorRules() {
        const bool previousConfined = state.cursorConfined;
        const bool previousVisible = cursorVisible;
        state.cursorConfined = IsCursorConfined(foregroundIsShell);
        CURSORINFO cursor{};
        cursor.cbSize = sizeof(cursor);
        cursorVisible = GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) != 0 &&
                        !(cursor.flags & CURSOR_SUPPRESSED) && cursor.hCursor;
        lastInputRuleCheck = GetTickCount64();
        return previousConfined != state.cursorConfined || previousVisible != cursorVisible;
    }

    void RefreshForegroundFacts() {
        HWND window = GetForegroundWindow();
        if (window) window = GetAncestor(window, GA_ROOT);
        DWORD processId = 0;
        if (window) GetWindowThreadProcessId(window, &processId);
        if (window != foregroundWindow || processId != foregroundProcessId)
            foregroundIsShell = IsShellWindowLike(window);
        if (processId != foregroundProcessId || !foregroundWindow) {
            InputTokenFacts foregroundTokenFacts;
            state.foregroundProcess = ProcessName(processId, foregroundTokenFacts);
            asyncButtonsReadable = CanReconcileAsyncButtons(ownTokenFacts, foregroundTokenFacts);
            state.foregroundElevated = processId != 0 && !asyncButtonsReadable &&
                (!foregroundTokenFacts.integrityKnown ||
                 (ownTokenFacts.integrityKnown && foregroundTokenFacts.integrity > ownTokenFacts.integrity));
            SetReleaseTimer();
            foregroundProcessId = processId;
            const auto name = FoldName(state.foregroundProcess);
            excluded = std::find(exclusions.begin(), exclusions.end(), name) != exclusions.end();
            alwaysAllowed = std::find(allowances.begin(), allowances.end(), name) != allowances.end();
        }
        foregroundWindow = window;
        foregroundEventWindow.store(window);
        state.fullscreen = IsFullscreenWindow(window, foregroundIsShell);
        RefreshCursorRules();
        inputDesktopActive = IsInputDesktopActive();
        if (!inputDesktopActive) RestoreAndClear();
    }

    void UpdateForeground() {
        RefreshForegroundFacts();
        UpdateEffect(true);
    }

    void RecountHeld() {
        unsigned held = 0;
        for (const auto& pair : devices) held |= pair.second;
        state.heldButtons = held;
        SetReleaseTimer();
    }

    void HandleMouse(const RAWINPUT& input) {
        if (input.header.dwType != RIM_TYPEMOUSE) return;
        const USHORT flags = input.data.mouse.usButtonFlags;
        if (suspended || state.sessionLocked) return;
        // Multiple movement samples in one buffered read need one renderer
        // wake. The renderer reads the latest screen position independently.
        if ((settings.highlightEnabled || settings.trailEnabled || settings.shakeStyle || settings.magnifierEnabled) &&
            servicesInitialized && MayApply() &&
            (input.data.mouse.lLastX || input.data.mouse.lLastY ||
             (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))) pointerActivityPending = true;
        if (!(flags & 0x03ff)) return;
        constexpr USHORT downs[] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN,
            RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
        constexpr USHORT ups[] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP,
            RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
        unsigned down = 0, up = 0;
        for (unsigned i = 0; i < std::size(downs); ++i) {
            if (flags & downs[i]) down |= 1u << i;
            if (flags & ups[i]) up |= 1u << i;
        }
        // WinEvents are the primary rule updates. When a rule can suppress a
        // click, verify it again on down in case an app omitted a resize event.
        // This fallback never runs for movement or release packets.
        if (down && (settings.pauseFullscreen || settings.pauseConfinedCursor || !exclusions.empty())) {
            RefreshForegroundFacts();
            if (!inputDesktopActive) { UpdateEffect(); return; }
        }
        else if (down) {
            RefreshCursorRules();
            if (!inputDesktopActive) inputDesktopActive = IsInputDesktopActive();
        }
        if (!inputDesktopActive) { UpdateEffect(); return; }
        const auto found = devices.find(input.header.hDevice);
        const unsigned previous = found == devices.end() ? 0 : found->second;
        const unsigned next = (previous | down) & ~up;
        const unsigned transitions = down & ~previous;
        if (previous == next && !transitions) return;
        if (previous != next) {
            if (next) devices[input.header.hDevice] = next;
            else if (found != devices.end()) devices.erase(found);
            RecountHeld();
        }
        // A packet may contain both down and up. It still produces one click
        // pulse/animation even though its final physical held mask is unchanged.
        if (down || up) shake.ClearTrace();
        if (transitions && settings.keyDisplayEnabled && settings.keyDisplayMouse && servicesInitialized && MayApply(true)) {
            constexpr unsigned buttonKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
            const unsigned modifiers = CurrentModifiers() | kModMouse;
            for (unsigned i = 0; i < std::size(buttonKeys); ++i)
                if (transitions & (1u << i)) visuals.Key(buttonKeys[i], 0, false, modifiers);
            if (AnyModifierDown()) modifierChordUsed = true;
        }
        UpdateEffect(false, transitions & 7u, transitions & settings.buttonMask);
    }

    void DrainInput(HRAWINPUT handle) {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        const UINT result = GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
        if (result == static_cast<UINT>(-1)) {
            Fail(ErrorText(L"Could not read mouse input"));
            return;
        }
        if (result >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE) && raw.header.dwType == RIM_TYPEMOUSE)
            HandleMouse(raw);
        else if (result >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD) && raw.header.dwType == RIM_TYPEKEYBOARD)
            HandleKeyboard(raw);
#ifdef _WIN64
        if (!bufferingAvailable) return;
        // GetMessage already removed the current WM_INPUT, so read it above
        // before draining subsequent samples. A bound keeps other messages fair.
        alignas(8) unsigned char buffer[16 * 1024];
        for (unsigned batch = 0; batch < 4; ++batch) {
            UINT bytes = sizeof(buffer);
            const UINT count = GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(buffer), &bytes,
                                                 sizeof(RAWINPUTHEADER));
            if (count == static_cast<UINT>(-1)) {
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) bufferingAvailable = false;
                break; // Standard WM_INPUT reads remain available as a fallback.
            }
            if (!count) break;
            auto* item = reinterpret_cast<PRAWINPUT>(buffer);
            const auto* end = buffer + sizeof(buffer);
            for (UINT i = 0; i < count; ++i) {
                const auto* begin = reinterpret_cast<const unsigned char*>(item);
                if (begin > end || static_cast<std::size_t>(end - begin) < sizeof(RAWINPUTHEADER) ||
                    item->header.dwSize < sizeof(RAWINPUTHEADER) ||
                    item->header.dwSize > static_cast<UINT>(end - begin)) break;
                if (item->header.dwType == RIM_TYPEMOUSE &&
                    item->header.dwSize >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE)) HandleMouse(*item);
                else if (item->header.dwType == RIM_TYPEKEYBOARD &&
                    item->header.dwSize >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD)) HandleKeyboard(*item);
                // Equivalent to NEXTRAWINPUTBLOCK on native Win64; avoids a
                // MinGW header version whose macro references undefined QWORD.
                constexpr std::uintptr_t alignment = sizeof(void*);
                const auto next = reinterpret_cast<std::uintptr_t>(item) + item->header.dwSize;
                item = reinterpret_cast<PRAWINPUT>((next + alignment - 1) & ~(alignment - 1));
            }
        }
#endif
    }

    void HandleInput(HRAWINPUT handle) {
        inputBatchActive = true;
        try {
            // One time/rule gate per raw batch, rather than per 1–8 kHz sample.
            // Button-down below still verifies foreground rules immediately.
            if (settings.enabled && settings.pauseConfinedCursor && !suspended && !state.sessionLocked &&
                GetTickCount64() - lastInputRuleCheck >= 200 && RefreshCursorRules()) UpdateEffect();
            DrainInput(handle);
        } catch (...) {
            inputBatchActive = false;
            pointerActivityPending = false;
            throw;
        }
        inputBatchActive = false;
        if (pointerActivityPending) {
            pointerActivityPending = false;
            if (servicesInitialized && MayApply()) visuals.PointerActivity();
            ObserveShakeMotion();
        }
        if (statusDirty) Publish();
    }

    void ReconcileRelease() {
        if (!state.heldButtons) { SetReleaseTimer(); return; }
        inputDesktopActive = IsInputDesktopActive();
        if (!inputDesktopActive || state.sessionLocked || suspended) {
            RestoreAndClear();
            UpdateEffect();
            return;
        }
        // A foreground event can be delayed, so verify the process boundary
        // immediately before trusting a zero asynchronous button-state result.
        DWORD processId = 0;
        if (HWND foreground = GetForegroundWindow()) GetWindowThreadProcessId(foreground, &processId);
        if (processId != foregroundProcessId) RefreshForegroundFacts();
        if (!asyncButtonsReadable) { SetReleaseTimer(); return; }
        const unsigned physical = AsyncHeldButtons();
        if ((state.heldButtons & physical) == state.heldButtons) return;
        for (auto it = devices.begin(); it != devices.end();) {
            it->second &= physical;
            if (!it->second) it = devices.erase(it);
            else ++it;
        }
        RecountHeld();
        UpdateEffect();
    }

    void QueueReload() {
        if (reloading) return;
        reloadPending = true;
        // This is a one-shot debounce, never an idle polling loop.
        if (HWND window = inputWindow.load()) {
            if (!SetTimer(window, kReloadTimer, 120, nullptr))
                Fail(ErrorText(L"Could not schedule a cursor scheme refresh"));
        }
    }

    void ReloadCursors() {
        KillTimer(inputWindow.load(), kReloadTimer);
        if (reloading) return;
        inputDesktopActive = IsInputDesktopActive();
        if (!inputDesktopActive || state.sessionLocked || suspended) {
            reloadPending = true;
            RestoreAndClear();
            UpdateEffect();
            return;
        }
        reloading = true;
        reloadPending = false;
        if (!RestoreAndClear()) { reloading = false; return; }
        if (!CursorEngine::RestoreSystemScheme()) {
            reloading = false;
            Fail(ErrorText(L"Could not reload the Windows cursor scheme"));
            return;
        }
        std::wstring error;
        bool success = engine.Reload(error);
        // A failed profile selection deliberately leaves the engine's previous
        // prepared appearance intact. Refresh must retry the UI's requested
        // profile, including recovery from a file removed since the last load.
        if (!success || !appearanceValid)
            success = engine.Configure(AppearanceFrom(settings), error);
        appearanceValid = success;
        engineActive = false;
        reloading = false;
        if (!success) { Fail(error); return; }
        state.ready = true;
        state.lastError.clear();
        state.cursorCount = engine.CursorCount();
        RefreshPreviews();
        UpdateForeground();
    }

    void DesktopChanged() {
        desktopQueued = false;
        ResetKeyState(); // Releases during lock/UAC desktops are never delivered here.
        RestoreAndClear();
        inputDesktopActive = IsInputDesktopActive();
        // Rebuild after returning from another/secure desktop, including after
        // a restoration attempt that Windows rejected while it was inactive.
        if (!inputDesktopActive || !state.ready) reloadPending = true;
        if (inputDesktopActive && reloadPending && !state.sessionLocked && !suspended) ReloadCursors();
        else UpdateForeground();
    }

    static void EventProc(void* context, DWORD event, HWND window,
                          LONG object, LONG child, DWORD) {
        Impl* self = static_cast<Impl*>(context);
        if (!self || self->stopRequested.load()) return;
        HWND target = self->inputWindow.load();
        if (!target) return;
        const auto queue = [target](std::atomic<bool>& pending, UINT message) {
            if (!pending.exchange(true) && !PostMessageW(target, message, 0, 0))
                pending.store(false);
        };
        if (event == EVENT_SYSTEM_FOREGROUND) {
            queue(self->foregroundQueued, kForegroundChanged);
        } else if (event == EVENT_SYSTEM_MOVESIZESTART || event == EVENT_SYSTEM_MOVESIZEEND) {
            queue(self->foregroundQueued, kForegroundChanged);
        } else if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
            queue(self->desktopQueued, kDesktopChanged);
        } else if (event == EVENT_OBJECT_LOCATIONCHANGE && object == OBJID_WINDOW && child == CHILDID_SELF) {
            if (window && (window == self->foregroundEventWindow.load() || window == GetForegroundWindow()))
                queue(self->foregroundGeometryQueued, kForegroundGeometryChanged);
        } else if ((event == EVENT_OBJECT_NAMECHANGE || event == EVENT_OBJECT_SHOW ||
                    event == EVENT_OBJECT_HIDE) && object == OBJID_CURSOR) {
            // Cursor location events are discarded by the window-only filter
            // above. A shape change while dragging may need a different role.
            queue(self->cursorQueued, kCursorChanged);
        }
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        try { return self->OnMessage(window, message, wParam, lParam); }
        catch (...) {
            try {
                self->Fail(L"An unexpected Windows event interrupted the cursor effect. Your cursors were restored.");
            } catch (...) {
                // Do not let an allocation failure cross a Win32 callback.
                CursorEngine::RestoreSystemScheme();
                PostQuitMessage(1);
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT OnMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_INPUT:
            HandleInput(reinterpret_cast<HRAWINPUT>(lParam));
            // Required cleanup for foreground RIM_INPUT, harmless for input sink.
            return DefWindowProcW(window, message, wParam, lParam);
        case WM_INPUT_DEVICE_CHANGE:
            if (wParam == GIDC_REMOVAL) {
                ResetKeyState(); // A detached keyboard never sends its releases.
                if (devices.erase(reinterpret_cast<HANDLE>(lParam))) { RecountHeld(); UpdateEffect(); }
            }
            return 0;
        case kSettingsChanged:
            ReadSettings(); return 0;
        case kRefreshCursors:
            ReloadCursors(); return 0;
        case kForegroundChanged:
            foregroundQueued = false;
            KillTimer(window, kForegroundTimer);
            foregroundTimerArmed = false;
            UpdateForeground(); return 0;
        case kForegroundGeometryChanged:
            foregroundGeometryQueued.store(false);
            if (!foregroundTimerArmed) {
                foregroundTimerArmed = SetTimer(window, kForegroundTimer, 60, nullptr) != 0;
                if (!foregroundTimerArmed) UpdateForeground();
            }
            return 0;
        case kDesktopChanged:
            DesktopChanged(); return 0;
        case kOptionalServiceChanged:
            Publish(); return 0;
        case kCursorChanged:
            cursorQueued = false;
            if (RefreshCursorRules()) UpdateEffect();
            if (servicesInitialized) visuals.PointerActivity();
            if ((engine.IsPressed() || shake.Active(Seconds())) && MayApply() && !reloading) {
                std::wstring error;
                LARGE_INTEGER begin{}; QueryPerformanceCounter(&begin);
                if (!engine.ApplyCurrent(error)) Fail(error);
                else { RecordUpdate(begin); Publish(); }
            }
            return 0;
        case WM_TIMER:
            if (wParam == kReleaseSafetyTimer) ReconcileRelease();
            else if (wParam == kForegroundTimer) {
                KillTimer(window, kForegroundTimer); foregroundTimerArmed = false; UpdateForeground();
            }
            else if (wParam == kReloadTimer) ReloadCursors();
            else if (wParam == kStatusTimer) {
                KillTimer(window, kStatusTimer);
                statusTimerArmed = false;
                NotifyStatus();
            } else if (wParam == kPulseTimer) {
                KillTimer(window, kPulseTimer);
                pulseTimerArmed = false;
                const ULONGLONG now = GetTickCount64();
                if (pulseUntil > now) {
                    pulseTimerArmed = SetTimer(window, kPulseTimer,
                        static_cast<UINT>(pulseUntil - now), nullptr) != 0;
                    if (!pulseTimerArmed) Fail(ErrorText(L"Could not finish the cursor pulse timer"));
                } else {
                    pulseUntil = 0;
                    UpdateEffect();
                }
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (!KnownUnrelatedSetting(wParam, lParam)) QueueReload();
            return 0;
        case WM_THEMECHANGED:
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            shakeMonitor = nullptr;
            QueueReload(); return 0;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK || wParam == WTS_CONSOLE_DISCONNECT || wParam == WTS_REMOTE_DISCONNECT) {
                state.sessionLocked = true; reloadPending = true; RestoreAndClear(); UpdateEffect();
            } else if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT || wParam == WTS_REMOTE_CONNECT) {
                state.sessionLocked = false; DesktopChanged();
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND) {
                suspended = true; reloadPending = true; RestoreAndClear(); UpdateEffect();
            } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                suspended = false; DesktopChanged();
            }
            return TRUE;
        case WM_QUERYENDSESSION:
            RestoreAndClear(); return TRUE;
        case WM_ENDSESSION:
            if (wParam) DestroyWindow(window);
            return 0;
        case WM_CLOSE:
            RestoreAndClear(); DestroyWindow(window); return 0;
        case WM_DESTROY:
            PostQuitMessage(0); return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    void SignalStartup(bool success, const std::wstring& error) {
        {
            std::lock_guard<std::mutex> lock(startupMutex);
            if (startupComplete) return;
            startupSuccess = success;
            startupError = error;
            startupComplete = true;
        }
        startupCondition.notify_all();
    }

    bool InitializeThread(std::wstring& error) {
        QueryPerformanceFrequency(&frequency);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ownTokenFacts = ReadInputTokenFacts(GetCurrentProcess());
        shakeScaleLibrary = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (shakeScaleLibrary) {
            const FARPROC address = GetProcAddress(shakeScaleLibrary, "GetDpiForMonitor");
            static_assert(sizeof(address) == sizeof(monitorDpi), "Windows function pointer size");
            std::memcpy(&monitorDpi, &address, sizeof(address));
        }
        ReadSettings(false);
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = kInputClass;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = ErrorText(L"Could not register the input window"); return false;
        }
        // A hidden top-level window receives session, theme and power broadcasts.
        // A message-only window would not receive those broadcasts.
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kInputClass,
                       L"ClickShade input", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                       cls.hInstance, this);
        if (!window) { error = ErrorText(L"Could not create the input window"); return false; }
        inputWindow.store(window);
        RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
        if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
            error = ErrorText(L"Could not subscribe to mouse input"); return false;
        }
        rawRegistered = true;
        if (!events.Start({{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
                           {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND},
                           {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
                           {EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE},
                           {EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE},
                           {EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH}},
                           EventProc, this, error)) return false;
        if (!WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION)) {
            error = ErrorText(L"Could not subscribe to Windows session events"); return false;
        }
        sessionRegistered = true;
        inputDesktopActive = IsInputDesktopActive();
        if (!engine.Initialize(error)) return false;
        // A missing custom profile remains editable in the running UI. It
        // pauses effects until a valid profile is selected, rather than making
        // an otherwise healthy input worker impossible to start.
        state.ready = engine.Configure(AppearanceFrom(settings), error);
        appearanceValid = state.ready;
        engineActive = false;
        state.cursorCount = engine.CursorCount();
        state.lastError = state.ready ? L"" : error;
        RefreshPreviews();
        std::wstring optionalError;
        try {
            if (!visuals.Start(settings, optionalError)) state.visualError = optionalError;
        } catch (...) { state.visualError = L"Pointer highlights could not be started."; }
        visuals.SetNotifyWindow(window, kOptionalServiceChanged);
        optionalError.clear();
        try {
            audio.SetNotifyWindow(window, kOptionalServiceChanged);
            if (!audio.Start(settings, optionalError)) state.audioError = optionalError;
        } catch (...) { state.audioError = L"Click audio could not be started."; }
        visuals.SetAllowed(false);
        audio.SetAllowed(false);
        servicesInitialized = true;
        servicesAllowed = false;
        keyServicesAllowed = false;
        RegisterKeyboard(KeyboardWanted());
        UpdateForeground();
        return true;
    }

    void CleanupThread() noexcept {
        try {
            visuals.SetAllowed(false);
            audio.SetAllowed(false);
        } catch (...) {}
        // Restore the native pointer before joining optional workers. A codec
        // or audio driver may take time to return from an in-flight call.
        std::wstring error;
        engine.Restore(error);
        engineActive = false;
        CancelPulse();
        CancelShake();
        if (shakeTimer) { CloseHandle(shakeTimer); shakeTimer = nullptr; }
        if (shakeScaleLibrary) { FreeLibrary(shakeScaleLibrary); shakeScaleLibrary = nullptr; monitorDpi = nullptr; }
        try {
            visuals.Stop();
            audio.Stop();
        } catch (...) {}
        servicesInitialized = false;
        events.Stop();
        HWND window = inputWindow.load();
        if (sessionRegistered && window) WTSUnRegisterSessionNotification(window);
        sessionRegistered = false;
        if (keyboardRegistered) {
            RAWINPUTDEVICE keyboard{0x01, 0x06, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard));
        }
        keyboardRegistered = false;
        if (rawRegistered) {
            RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
        }
        rawRegistered = false;
        if (window && IsWindow(window)) DestroyWindow(window);
        inputWindow.store(nullptr);
        devices.clear();
        releaseTimerArmed = false;
        try {
            state.heldButtons = 0;
            state.pressed = false;
            state.ready = false;
            state.stateText = L"Stopped. Your normal cursors are restored.";
            Publish(false);
        } catch (...) {}
    }

    void ThreadMain() noexcept {
        threadId.store(GetCurrentThreadId());
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // Create the thread queue.
        try {
            std::wstring error;
            if (!InitializeThread(error)) {
                state.lastError = error;
                SignalStartup(false, error);
            } else if (stopRequested.load()) {
                SignalStartup(false, L"The cursor engine startup was cancelled.");
            } else {
                SignalStartup(true, {});
                bool quit = false;
                while (!quit && !stopRequested.load()) {
                    for (unsigned count = 0; count < 32 &&
                         PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                        if (message.message == WM_QUIT) { quit = true; break; }
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                        if (shakeTimerArmed && Seconds() >= shakeDeadline) break;
                    }
                    if (quit || stopRequested.load()) break;
                    if (shakeTimerArmed && Seconds() >= shakeDeadline) {
                        shakeTimerArmed = false;
                        TickShake();
                        continue;
                    }
                    const DWORD handles = shakeTimerArmed ? 1u : 0u;
                    const DWORD result = MsgWaitForMultipleObjectsEx(handles, handles ? &shakeTimer : nullptr,
                        INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    if (result == WAIT_FAILED) {
                        Fail(ErrorText(L"The mouse input message wait failed")); break;
                    }
                    if (handles && result == WAIT_OBJECT_0) {
                        shakeTimerArmed = false;
                        TickShake();
                    }
                }
            }
        } catch (...) {
            try {
                Fail(L"The cursor engine could not continue. Your cursors were restored.");
                SignalStartup(false, state.lastError);
            } catch (...) {}
        }
        CleanupThread();
        threadId.store(0);
    }
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() { Stop(); }

bool Runtime::Start(HWND notifyWindow, UINT notifyMessage, const Settings& settings,
                    std::wstring& error) {
    Stop();
    // A fresh implementation also clears stale event/debounce state on restart.
    impl_ = std::make_unique<Impl>();
    impl_->notifyWindow = notifyWindow;
    impl_->notifyMessage = notifyMessage;
    impl_->pendingSettings = settings;
    try { impl_->worker = std::thread([this] { impl_->ThreadMain(); }); }
    catch (...) { error = L"Could not start the mouse input thread."; return false; }
    std::unique_lock<std::mutex> lock(impl_->startupMutex);
    if (!impl_->startupCondition.wait_for(lock, std::chrono::seconds(10),
                                        [this] { return impl_->startupComplete; })) {
        error = L"The cursor engine did not finish starting within 10 seconds.";
        impl_->stopRequested.store(true);
        if (HWND window = impl_->inputWindow.load()) PostMessageW(window, WM_CLOSE, 0, 0);
        else if (DWORD id = impl_->threadId.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
        return false;
    }
    const bool success = impl_->startupSuccess;
    error = impl_->startupError;
    lock.unlock();
    if (!success) Stop();
    return success;
}

void Runtime::Stop() {
    if (!impl_ || !impl_->worker.joinable()) return;
    impl_->stopRequested.store(true);
    HWND window = impl_->inputWindow.load();
    if (!window || !PostMessageW(window, WM_CLOSE, 0, 0)) {
        if (DWORD id = impl_->threadId.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
    }
    impl_->worker.join();
}

void Runtime::UpdateSettings(const Settings& settings) {
    bool post = false;
    {
        std::lock_guard<std::mutex> lock(impl_->settingsMutex);
        impl_->pendingSettings = settings;
        if (!impl_->settingsQueued) {
            impl_->settingsQueued = true;
            post = true;
        }
    }
    if (post) {
        const HWND window = impl_->inputWindow.load();
        if (!window || !PostMessageW(window, kSettingsChanged, 0, 0)) {
            std::lock_guard<std::mutex> lock(impl_->settingsMutex);
            impl_->settingsQueued = false;
        }
    }
}

void Runtime::RefreshCursors() {
    if (HWND window = impl_->inputWindow.load()) PostMessageW(window, kRefreshCursors, 0, 0);
}

void Runtime::PreviewSound(unsigned buttonMask) {
    if (impl_) impl_->audio.Preview(buttonMask & 7u);
}

void Runtime::PreviewKeyboardSound() {
    if (impl_) impl_->audio.PreviewKey();
}

void Runtime::PreviewKeyDisplay() {
    if (impl_) impl_->visuals.PreviewKey();
}

RuntimeStatus Runtime::Snapshot() const {
    RuntimeStatus result;
    {
        std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
        result = impl_->snapshot;
        // A queued notification promises the latest snapshot, not one specific
        // click. Clear under the same lock used by Publish to avoid a lost wake.
        impl_->statusNotificationQueued = false;
    }
    // Optional worker errors are collected only when the UI requests a
    // snapshot; raw mouse processing never waits on their diagnostic mutexes.
    const auto visualError = impl_->visuals.LastError();
    result.locatorScale = impl_->locatorScale.load(std::memory_order_relaxed);
    const auto audioError = impl_->audio.LastError();
    if (!visualError.empty()) result.visualError = visualError;
    if (!audioError.empty()) result.audioError = audioError;
    return result;
}

HCURSOR Runtime::CopyPreview(bool dark) const {
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex);
    HCURSOR cursor = dark ? impl_->previewDark : impl_->previewNormal;
    return cursor ? CopyCursor(cursor) : nullptr;
}

} // namespace clickshade
