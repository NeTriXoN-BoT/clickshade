// Entry point and window procedures for the Windows settings window. The
// window body lives in ui_layout.cpp, ui_paint.cpp and ui_actions.cpp.
#include "ui_common.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <cwchar>

namespace clickshade::ui {

LRESULT CALLBACK ToggleProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        app->DrawToggle(window, dc); EndPaint(window, &paint); return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PRINTCLIENT) { app->DrawToggle(window, reinterpret_cast<HDC>(wparam)); return 0; }
    if (message == WM_MOUSEWHEEL) { return SendMessageW(GetParent(window), message, wparam, lparam); }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ToggleProc, id);
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == BM_SETCHECK || message == BM_SETSTATE || message == WM_ENABLE ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(window, nullptr, FALSE);
    return result;
}

LRESULT CALLBACK SliderProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    if (message == WM_PAINT) { PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps); app->DrawSlider(window, dc); EndPaint(window, &ps); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PRINTCLIENT) { app->DrawSlider(window, reinterpret_cast<HDC>(wparam)); return 0; }
    // The wheel scrolls the page; it must never nudge a value by accident.
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) return SendMessageW(GetParent(window), message, wparam, lparam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, SliderProc, id);
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == TBM_SETPOS ||
        message == WM_MOUSEMOVE || message == WM_KEYDOWN || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_SIZE)
        InvalidateRect(window, nullptr, FALSE);
    return result;
}

// Edits and dropdowns hand the wheel to the page as well; a closed dropdown
// otherwise changes the selected sound while scrolling past it.
LRESULT CALLBACK WheelProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        wchar_t type[32]{}; GetClassNameW(window, type, 32);
        const bool openList = _wcsicmp(type, L"COMBOBOX") == 0 && SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0);
        if (!openList) return SendMessageW(GetParent(window), message, wparam, lparam);
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, WheelProc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK FocusProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    if (message == WM_SETFOCUS) app->EnsureVisible(window);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(app->panel, nullptr, FALSE);
    wchar_t type[32]{};
    if (message == WM_KEYDOWN || message == WM_GETDLGCODE) GetClassNameW(window, type, 32);
    if (std::wstring(type) == L"Button") {
        if (message == WM_KEYDOWN && app->ScrollKey(static_cast<UINT>(wparam))) return 0;
        if (message == WM_GETDLGCODE && lparam) {
            const auto* key = reinterpret_cast<MSG*>(lparam);
            if (key->message == WM_KEYDOWN && (key->wParam == VK_PRIOR || key->wParam == VK_NEXT || key->wParam == VK_HOME || key->wParam == VK_END))
                return DefSubclassProc(window, message, wparam, lparam) | DLGC_WANTMESSAGE;
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, FocusProc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK ShortcutProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    auto* app = reinterpret_cast<App*>(data);
    const int row = GetDlgCtrlID(window) - HotkeyFieldBase;
    if (app->capturingShortcut == row) {
        if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
            UINT modifiers = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
            app->CaptureShortcut(static_cast<UINT>(wparam), modifiers); return 0;
        }
        if (message == WM_KILLFOCUS && reinterpret_cast<HWND>(wparam) != app->controls[HotkeyRecordBase + row])
            app->CancelShortcutCapture();
    }
    if ((message == WM_CHAR || message == WM_SYSCHAR) && (app->capturingShortcut == row || app->suppressShortcutCharacters)) return 0;
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && app->capturingShortcut < 0) app->suppressShortcutCharacters = false;
    if (message == WM_KILLFOCUS) app->suppressShortcutCharacters = false;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ShortcutProc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}

namespace {
// Control notifications arrive at the control's parent. The panel forwards
// them to the App; a C++ exception in a handler becomes a status note.
LRESULT HandleControlMessage(App* app, HWND window, UINT message, WPARAM wparam, LPARAM lparam, bool& handled) {
    handled = true;
    switch (message) {
    case WM_COMMAND:
        if (app->exiting) return 0;
        if (HIWORD(wparam) == BN_CLICKED) app->Command(LOWORD(wparam));
        else if (HIWORD(wparam) == CBN_SELCHANGE) {
            if (LOWORD(wparam) == KeyboardSet) app->KeyboardSetChanged();
            else app->SoundPresetChanged(LOWORD(wparam));
        }
        else if (HIWORD(wparam) == EN_KILLFOCUS) app->NumberChanged(LOWORD(wparam));
        else if (HIWORD(wparam) == EN_CHANGE && !app->syncing && LOWORD(wparam) >= HotkeyFieldBase && LOWORD(wparam) < HotkeyFieldBase + kHotkeys.size()) {
            app->hotkeyDraftDirty = true;
            if (app->capturingShortcut < 0) {
                app->shortcutError = false; app->shortcutStatus = L"Choose Apply shortcuts to save your changes.";
                app->Layout();
            }
        }
        return 0;
    case WM_HSCROLL:
        if (lparam) app->SliderChanged(GetDlgCtrlID(reinterpret_cast<HWND>(lparam)), LOWORD(wparam) == TB_THUMBTRACK);
        return 0;
    case WM_DRAWITEM: app->DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (item->CtlType == ODT_COMBOBOX) { item->itemHeight = app->S(24); return TRUE; }
        break;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wparam), app->colors.text);
        SetBkColor(reinterpret_cast<HDC>(wparam), app->colors.edit);
        return reinterpret_cast<LRESULT>(app->editBrush);
    case WM_MOUSEWHEEL: app->ScrollBy(-GET_WHEEL_DELTA_WPARAM(wparam) * app->S(56) / WHEEL_DELTA); return 0;
    default: break;
    }
    (void)window;
    handled = false;
    return 0;
}

LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    try {
        bool handled = false;
        const LRESULT result = HandleControlMessage(app, window, message, wparam, lparam, handled);
        if (handled) return result;
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps);
            RECT rect{}; GetClientRect(window, &rect);
            HDC buffer = app->Buffer(dc, rect.right, rect.bottom, app->panelBuffer, app->panelBitmap, app->panelOriginal, app->panelSize);
            if (buffer) {
                const int saved = SaveDC(buffer);
                IntersectClipRect(buffer, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
                app->PaintPanel(buffer); RestoreDC(buffer, saved);
                BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                    buffer, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            } else app->PaintPanel(dc);
            EndPaint(window, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_KEYDOWN: if (app->ScrollKey(static_cast<UINT>(wparam))) return 0; break;
        case WM_LBUTTONDOWN: SetFocus(window); break; // Clicking empty space drops focus from edits.
        default: break;
        }
    } catch (const std::exception& error) {
        wchar_t detail[256]{}; MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, detail, 255);
        NoteInternalError(std::wstring(L"A settings action failed: ") + detail);
        app->crashNotice = TakeCrashNotice();
    } catch (...) {
        NoteInternalError(L"A settings action failed unexpectedly. Your preferences were not changed.");
        app->crashNotice = TakeCrashNotice();
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    if (app->taskbarCreated && message == app->taskbarCreated) { app->AddTray(); return 0; }
    try {
        bool handled = false;
        if (message != WM_MOUSEWHEEL) {
            const LRESULT result = HandleControlMessage(app, window, message, wparam, lparam, handled);
            if (handled) return result;
        }
        switch (message) {
        case WM_CREATE: {
            if (const UINT actualDpi = GetDpiForWindow(window)) app->systemDpi = static_cast<int>(actualDpi);
            // Lower-integrity instances (a second launch, --restore, --quit)
            // must still reach an elevated settings window.
            for (UINT allowed : {kShowSettings, kRestoreAndPause, kQuitRequest})
                ChangeWindowMessageFilterEx(window, allowed, MSGFLT_ALLOW, nullptr);
            app->Scale(false);
            app->panel = CreateWindowExW(WS_EX_CONTROLPARENT, kPanelClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                0, 0, 10, 10, window, nullptr, GetModuleHandleW(nullptr), app);
            app->CreateControls();
            return 0;
        }
        case WM_TIMER:
            if (wparam == kSaveTimer) { app->FlushSettings(); return 0; }
            if (wparam == kSpectrumTimer) { app->TickSpectrum(); return 0; }
            if (wparam == kHealthyTimer) { KillTimer(window, kHealthyTimer); ResetRestartGeneration(); return 0; }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps);
            RECT rect{}; GetClientRect(window, &rect);
            HDC buffer = app->Buffer(dc, rect.right, rect.bottom, app->paintBuffer, app->paintBitmap, app->paintOriginal, app->paintSize);
            if (buffer) {
                const int saved = SaveDC(buffer);
                IntersectClipRect(buffer, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
                app->Paint(buffer); RestoreDC(buffer, saved);
                BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                    buffer, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            } else app->Paint(dc);
            EndPaint(window, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) { app->CancelShortcutCapture(); ShowWindow(window, SW_HIDE); app->StopSpectrum(); }
            else app->Layout();
            return 0;
        case WM_SHOWWINDOW: if (wparam) app->StartSpectrum(); else app->StopSpectrum(); break;
        case WM_KEYDOWN: if (app->ScrollKey(static_cast<UINT>(wparam))) return 0; break;
        case WM_MOUSEWHEEL: app->ScrollBy(-GET_WHEEL_DELTA_WPARAM(wparam) * app->S(56) / WHEEL_DELTA); return 0;
        case WM_VSCROLL: {
            RECT client{}; GetClientRect(app->panel, &client);
            switch (LOWORD(wparam)) {
            case SB_LINEUP: app->scroll -= app->S(26); break;
            case SB_LINEDOWN: app->scroll += app->S(26); break;
            case SB_PAGEUP: app->scroll -= client.bottom; break;
            case SB_PAGEDOWN: app->scroll += client.bottom; break;
            case SB_TOP: app->scroll = 0; break;
            case SB_BOTTOM: app->scroll = app->S(app->contentHeight); break;
            default: break;
            }
            app->Layout(); return 0;
        }
        case WM_DPICHANGED: {
            const int oldSystem = app->systemDpi, oldEffective = app->dpi;
            app->systemDpi = HIWORD(wparam);
            const auto rect = *reinterpret_cast<RECT*>(lparam);
            // First move to the new monitor so automatic resolution zoom uses its
            // work area. The suggested bounds already include Windows DPI scaling.
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
            app->Scale(false);
            const int extraNumerator = app->dpi * oldSystem, extraDenominator = oldEffective * app->systemDpi;
            app->FitWindow(rect.left, rect.top,
                MulDiv(rect.right - rect.left, extraNumerator, extraDenominator),
                MulDiv(rect.bottom - rect.top, extraNumerator, extraDenominator));
            app->Fonts(); app->Layout(); return 0;
        }
        case WM_EXITSIZEMOVE: case WM_DISPLAYCHANGE: app->Scale(true); app->Layout(); return 0;
        case WM_SETTINGCHANGE: app->StopSpectrum(); app->StartSpectrum(); break;
        case WM_GETMINMAXINFO: {
            auto limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize.x = app->S(640); limits->ptMinTrackSize.y = app->S(520); return 0;
        }
        case WM_HOTKEY: if (wparam >= kHotkeyBase && wparam < kHotkeyBase + kHotkeys.size()) app->HotkeyAction(static_cast<unsigned>(wparam - kHotkeyBase)); return 0;
        case kStatusChanged: app->ReadStatus(); return 0;
        case kShowSettings: app->Show(); return 0;
        case kRestoreAndPause: app->RestoreAndPause(); return 0;
        case kQuitRequest: app->Exit(); return 0;
        case kTrayMessage:
            if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT || LOWORD(lparam) == WM_LBUTTONUP) app->Show();
            else if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) app->TrayMenu();
            return 0;
        case WM_CLOSE: app->CancelShortcutCapture(); app->FlushSettings(); ShowWindow(window, SW_HIDE); app->StopSpectrum(); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION: if (wparam) app->Exit(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
        }
    } catch (const std::exception& error) {
        wchar_t detail[256]{}; MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, detail, 255);
        NoteInternalError(std::wstring(L"A settings action failed: ") + detail);
        app->crashNotice = TakeCrashNotice();
        InvalidateRect(window, nullptr, FALSE);
    } catch (...) {
        NoteInternalError(L"A settings action failed unexpectedly. Your preferences were not changed.");
        app->crashNotice = TakeCrashNotice();
        InvalidateRect(window, nullptr, FALSE);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct Arguments {
    bool tray = false, restore = false, quit = false, noElevate = false, help = false, version = false;
    unsigned afterCrash = 0;
    DWORD replacePid = 0;
    HANDLE guardParent = nullptr, guardEvent = nullptr;
    bool guard = false, invalid = false;
};

Arguments ParseArguments() {
    Arguments result;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const auto number = [](const wchar_t* text, unsigned long long& value) {
        wchar_t* end = nullptr; value = wcstoull(text, &end, 10); return end && !*end && end != text;
    };
    for (int n = 1; argv && n < argc; ++n) {
        const std::wstring argument = argv[n];
        unsigned long long value = 0;
        if (argument == L"--guard" && n + 2 < argc) {
            unsigned long long first = 0, second = 0;
            if (!number(argv[n + 1], first) || !number(argv[n + 2], second)) { result.invalid = true; break; }
            result.guard = true;
            result.guardParent = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(first));
            result.guardEvent = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(second));
            n += 2;
        } else if (argument == L"--restore") result.restore = true;
        else if (argument == L"--quit") result.quit = true;
        else if (argument == L"--tray") result.tray = true;
        else if (argument == L"--no-elevate") result.noElevate = true;
        else if (argument == L"--help" || argument == L"-h" || argument == L"/?") result.help = true;
        else if (argument == L"--version") result.version = true;
        else if (argument == L"--after-crash" && n + 1 < argc && number(argv[n + 1], value)) { result.afterCrash = static_cast<unsigned>(std::min<unsigned long long>(value, 99)); ++n; }
        else if (argument == L"--replace" && n + 1 < argc && number(argv[n + 1], value)) { result.replacePid = static_cast<DWORD>(value); ++n; }
    }
    if (argv) LocalFree(argv);
    return result;
}

void WaitForReplacedProcess(DWORD pid) {
    if (!pid || pid == GetCurrentProcessId()) return;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return;
    WaitForSingleObject(process, 10000);
    CloseHandle(process);
}
// The running instance's settings window, waiting briefly for an instance
// that holds the mutex but is still creating its window. Every ClickShade
// build shares the class name and the mutex, so an old and a new copy can
// never run side by side.
HWND FindRunningInstance(DWORD timeoutMs = 0) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) return existing;
        if (GetTickCount64() >= deadline) return nullptr;
        Sleep(50);
    }
}
} // namespace
} // namespace clickshade::ui

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace clickshade;
    using namespace clickshade::ui;
    const Arguments arguments = ParseArguments();
    if (arguments.invalid) return 2;
    if (arguments.guard) return RunRecoveryGuard(arguments.guardParent, arguments.guardEvent);
    if (arguments.help || arguments.version) {
        const std::wstring text = std::wstring(L"ClickShade ") + kClickShadeVersionW + L"\n\n"
            L"ClickShade.exe            open the settings window\n"
            L"ClickShade.exe --tray     start in the tray\n"
            L"ClickShade.exe --restore  restore cursors and pause the running instance\n"
            L"ClickShade.exe --quit     quit the running instance\n"
            L"ClickShade.exe --no-elevate  start without the administrator relaunch\n\n"
            L"Settings: " + SettingsPath() + (PortableMode() ? L" (portable mode)" : L"");
        MessageBoxW(nullptr, text.c_str(), L"ClickShade", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (arguments.restore) {
        if (HWND existing = FindRunningInstance()) PostMessageW(existing, kRestoreAndPause, 0, 0);
        return CursorEngine::RestoreSystemScheme() ? 0 : 1;
    }
    if (arguments.quit) {
        HWND existing = FindRunningInstance();
        if (existing) PostMessageW(existing, kQuitRequest, 0, 0);
        return existing ? 0 : 1;
    }
    WaitForReplacedProcess(arguments.replacePid);

    CrashGuardOptions crashOptions;
    crashOptions.restartGeneration = arguments.afterCrash;
    InstallCrashGuard(crashOptions);

    // One instance per desktop session. A second launch, whatever its version
    // or folder, only brings the running settings window to the front.
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\ClickShade.Instance.v1");
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindRunningInstance(3000)) PostMessageW(existing, kShowSettings, 0, 0);
        CloseHandle(singleton); return 0;
    }
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&common);
    App app;
    app.elevated = IsProcessElevated();
    app.crashNotice = TakeCrashNotice();
    // Administrator mode relaunches before any window exists so the user sees
    // one prompt and one window. A declined prompt continues unelevated.
    if (app.settings.runAsAdministrator && !app.elevated && !arguments.noElevate) {
        std::wstring error;
        ReleaseMutex(singleton); CloseHandle(singleton); singleton = nullptr;
        if (RelaunchElevated(error)) return 0;
        singleton = CreateMutexW(nullptr, TRUE, L"Local\\ClickShade.Instance.v1");
        if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) { if (singleton) CloseHandle(singleton); return 0; }
        app.compatibilityNote = error + L" Effects over administrator windows stay limited this session.";
    }
    HDC screen = GetDC(nullptr); app.systemDpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(nullptr, screen); app.Scale(false);
    app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW panelClass{}; panelClass.cbSize = sizeof(panelClass);
    panelClass.hInstance = instance; panelClass.lpfnWndProc = PanelProc; panelClass.lpszClassName = kPanelClass;
    panelClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&panelClass)) { CloseHandle(singleton); return 1; }
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls);
    cls.hInstance = instance; cls.lpfnWndProc = WindowProc; cls.lpszClassName = kWindowClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = app.windowIcon; cls.hIconSm = app.smallIcon;
    cls.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&cls)) { CloseHandle(singleton); return 1; }
    RECT work = app.WorkArea();
    const int width = std::min(app.S(kDefaultWindowWidth), static_cast<int>(work.right - work.left) - 32);
    const int height = std::min(app.S(kDefaultWindowHeight), static_cast<int>(work.bottom - work.top) - 60);
    HWND window = CreateWindowExW(0, kWindowClass, L"ClickShade",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance, &app);
    if (!window) { CloseHandle(singleton); return 1; }
    // A secondary monitor may use a different DPI from GetDeviceCaps above.
    // Reconcile the initial outer bounds after the real window DPI is known.
    app.InitialWindowSize();
    app.ApplyTheme();
    app.AddTray();
    app.SetupShortcuts();
    app.ReconcileStartupRegistration();
    app.StartEngine();
    app.Layout();
    SetTimer(window, kHealthyTimer, 90000, nullptr);
    if ((!arguments.tray && !app.settings.startMinimized) || !app.started || app.crashNotice.present) {
        if (app.crashNotice.present) app.page = kAppearancePage; // Show the recovery note on About.
        ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window);
        app.StartSpectrum();
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    app.runtime.Stop(); app.guard.StopCleanly();
    ReleaseMutex(singleton); CloseHandle(singleton);
    return 0;
}
