#include "ui_common.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

namespace clickshade::ui {
namespace {
struct ComRelease { void operator()(IUnknown* value) const noexcept { if (value) value->Release(); } };
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ComApartment() = default;
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

// Shell item dialog rooted at a folder, for sounds (files) or sets (folders).
std::wstring ShellPick(HWND owner, const std::wstring& initial, bool folders, const wchar_t* title, std::wstring& error) {
    ComApartment apartment;
    HRESULT result = apartment.result;
    if (result == RPC_E_CHANGED_MODE) result = S_OK;
    IFileOpenDialog* raw = nullptr;
    if (SUCCEEDED(result)) result = CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IFileOpenDialog), reinterpret_cast<void**>(&raw));
    std::unique_ptr<IFileOpenDialog, ComRelease> dialog(raw);
    if (SUCCEEDED(result)) {
        FILEOPENDIALOGOPTIONS options{};
        result = dialog->GetOptions(&options);
        if (SUCCEEDED(result)) result = dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
            FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | (folders ? FOS_PICKFOLDERS : 0));
        const COMDLG_FILTERSPEC filters[]{{L"Audio files", L"*.wav;*.mp3;*.wma;*.flac;*.m4a;*.aac"}, {L"All files", L"*.*"}};
        if (SUCCEEDED(result) && !folders) result = dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        if (SUCCEEDED(result)) result = dialog->SetTitle(title);
        if (SUCCEEDED(result)) result = dialog->SetFileName(L"");
        std::wstring start = initial;
        DWORD attributes = GetFileAttributesW(start.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) start = SoundDirectory();
        attributes = GetFileAttributesW(start.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) start = ExecutableDirectory();
        IShellItem* rawFolder = nullptr;
        if (SUCCEEDED(result)) result = SHCreateItemFromParsingName(start.c_str(), nullptr, __uuidof(IShellItem), reinterpret_cast<void**>(&rawFolder));
        std::unique_ptr<IShellItem, ComRelease> folder(rawFolder);
        // SetFolder intentionally overrides the last-used directory so every
        // sound dialog begins in the bundled library.
        if (SUCCEEDED(result)) result = dialog->SetFolder(folder.get());
        if (SUCCEEDED(result)) result = dialog->Show(owner);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
        IShellItem* rawSelected = nullptr;
        if (SUCCEEDED(result)) result = dialog->GetResult(&rawSelected);
        std::unique_ptr<IShellItem, ComRelease> selected(rawSelected);
        PWSTR name = nullptr;
        if (SUCCEEDED(result)) result = selected->GetDisplayName(SIGDN_FILESYSPATH, &name);
        if (SUCCEEDED(result)) {
            const std::wstring path = name ? name : L"";
            CoTaskMemFree(name); return path;
        }
        if (name) CoTaskMemFree(name);
    }
    error = L"The file picker could not open (Windows error " + std::to_wstring(static_cast<unsigned long>(result)) + L").";
    return {};
}
} // namespace

std::wstring App::ControlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::vector<wchar_t> text(static_cast<std::size_t>(length) + 1);
    GetWindowTextW(control, text.data(), length + 1); return text.data();
}

SIZE App::PreviewSize(HCURSOR cursor) {
    SIZE result{32, 32};
    ICONINFO info{};
    if (!cursor || !GetIconInfo(cursor, &info)) return result;
    BITMAP bitmap{};
    const HBITMAP image = info.hbmColor ? info.hbmColor : info.hbmMask;
    if (image && GetObjectW(image, sizeof(bitmap), &bitmap)) {
        const LONG height = info.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
        if (bitmap.bmWidth > 0 && height > 0) result = {bitmap.bmWidth, height};
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return result;
}

void App::RememberPalette() {
    if (settings.uiDarkMode) { settings.uiDarkBackground = settings.uiBackground; settings.uiDarkAccent = settings.uiAccent; }
    else { settings.uiLightBackground = settings.uiBackground; settings.uiLightAccent = settings.uiAccent; }
}

void App::SetThemeMode(bool dark) {
    if (settings.uiDarkMode == dark) return;
    RememberPalette(); settings.uiDarkMode = dark;
    settings.uiBackground = dark ? settings.uiDarkBackground : settings.uiLightBackground;
    settings.uiAccent = dark ? settings.uiDarkAccent : settings.uiLightAccent;
    ApplyTheme(); Persist(false); Layout();
}

void App::Theme(COLORREF base, COLORREF accent, bool dark) {
    RememberPalette(); settings.uiDarkMode = dark;
    settings.uiBackground = base; settings.uiAccent = accent;
    RememberPalette();
    ApplyTheme(); Persist(false); Layout();
}

void App::FlushSettings() {
    KillTimer(window, kSaveTimer);
    if (!pendingSave) return;
    std::wstring error;
    if (SaveSettings(settings, error)) pendingSave = false;
    else MessageBoxW(window, error.c_str(), L"ClickShade", MB_ICONWARNING);
}

void App::Persist(bool readButtons) {
    if (exiting || syncing) return;
    if (readButtons) {
        for (const auto& spec : kToggles)
            if (spec.id != Startup) settings.*spec.member = Checked(spec.id);
        settings.buttonMask = 0;
        const std::array<int, 5> ids{Left, Right, Middle, Side1, Side2};
        for (unsigned n = 0; n < ids.size(); ++n) if (Checked(ids[n])) settings.buttonMask |= 1u << n;
    }
    RememberPalette();
    pendingSave = true; SetTimer(window, kSaveTimer, 300, nullptr);
    if (started) runtime.UpdateSettings(settings);
    UpdateTray();
    const Settings defaults;
    for (const auto& spec : kSliders) {
        const bool enabled = SliderEnabled(spec.id);
        EnableWindow(controls[spec.id], enabled);
        EnableWindow(controls[spec.id + kValueOffset], enabled);
        EnableWindow(controls[spec.id + kResetOffset], enabled && settings.*(spec.member) != defaults.*(spec.member));
    }
    for (int n = 0; n < kCardResetCount && n < static_cast<int>(cardChanged.size()); ++n)
        EnableWindow(controls[kCardResetBase + n], cardChanged[n]());
    if (IsWindowVisible(window)) { InvalidateRect(window, nullptr, FALSE); InvalidateRect(panel, nullptr, FALSE); }
}

void App::SliderChanged(int id, bool tracking) {
    if (syncing) return;
    const auto* spec = Slider(id); if (!spec) return;
    const int value = static_cast<int>(SendMessageW(controls[id], TBM_GETPOS, 0, 0));
    SetWindowTextW(controls[id + kValueOffset], std::to_wstring(value).c_str());
    if (tracking && !spec->live) return;
    settings.*(spec->member) = value;
    Persist(false);
    if (id == UiScale) Scale(true);
}

void App::NumberChanged(int id) {
    if (syncing) return;
    const auto* spec = Slider(id - kValueOffset); if (!spec || !SliderEnabled(spec->id)) return;
    BOOL valid = FALSE; const UINT value = GetDlgItemInt(panel, id, &valid, FALSE);
    const int result = valid ? std::clamp(static_cast<int>(std::min(value, 100000u)), spec->minimum, spec->maximum) : settings.*(spec->member);
    settings.*(spec->member) = result;
    SendMessageW(controls[spec->id], TBM_SETPOS, TRUE, result);
    SetWindowTextW(controls[id], std::to_wstring(result).c_str());
    Persist(false); if (spec->id == UiScale) Scale(true);
}

void App::ResetSlider(int id) {
    const auto* spec = Slider(id); if (!spec) return;
    const Settings defaults;
    settings.*(spec->member) = defaults.*(spec->member);
    SyncControls(); Persist(false);
    if (id == UiScale) Scale(true);
}

void App::ResetCard(int index) {
    if (index < 0 || index >= static_cast<int>(cardResets.size())) return;
    cardResets[static_cast<std::size_t>(index)]();
    pressedAutomatically = false;
    SyncControls(); Persist(false);
    SetWindowTextW(controls[Excluded], settings.excludedProcesses.c_str());
    SetWindowTextW(controls[Allowed], settings.allowedProcesses.c_str());
    if (page == kAppearancePage) { ApplyTheme(); Scale(true); }
    if (page == kShortcutsPage) { hotkeyDraftDirty = false; SetupShortcuts(); SyncControls(); }
    Layout();
}

std::wstring App::PickFile(int kind) {
    std::vector<wchar_t> path(32768, 0);
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = kind == 2 ? L"Static Windows cursors (*.cur)\0*.cur\0\0" : L"Applications (*.exe)\0*.exe\0\0";
    dialog.lpstrFile = path.data(); dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = kind == 2 ? L"Choose a static cursor" : L"Choose an application";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) return path.data();
    if (const DWORD code = CommDlgExtendedError()) {
        const auto error = L"The file picker could not open (Windows error " + std::to_wstring(code) + L").";
        MessageBoxW(window, error.c_str(), L"ClickShade", MB_ICONWARNING);
    }
    return {};
}

std::wstring App::PickSound() {
    std::wstring error;
    const auto path = ShellPick(window, SoundDirectory(), false, L"Choose a click sound", error);
    if (!error.empty()) MessageBoxW(window, error.c_str(), L"ClickShade", MB_ICONWARNING);
    return path;
}

std::wstring App::PickFolder() {
    std::wstring error;
    const auto path = ShellPick(window, KeyboardSoundDirectory(), true, L"Choose a keyboard sound set folder", error);
    if (!error.empty()) MessageBoxW(window, error.c_str(), L"ClickShade", MB_ICONWARNING);
    return path;
}

void App::SelectCursorFile(const std::wstring& path, bool pressed) {
    if (pressed) { settings.pressedCursorPath = path; pressedAutomatically = false; }
    else {
        settings.releasedCursorPath = path;
        if (settings.pressedCursorPath.empty() || pressedAutomatically) {
            std::wstring companion; bool selectedIsPressed = false;
            if (FindCursorCompanion(path, companion, selectedIsPressed)) {
                settings.releasedCursorPath = selectedIsPressed ? companion : path;
                settings.pressedCursorPath = selectedIsPressed ? path : companion;
                pressedAutomatically = true;
            } else {
                settings.pressedCursorPath.clear(); pressedAutomatically = false;
            }
        }
    }
    SyncControls(); Persist(false);
}

void App::BrowseCursor(bool pressed) {
    const auto path = PickFile(2); if (!path.empty()) SelectCursorFile(path, pressed);
}

void App::EffectPreset(int choice) {
    settings.highlightEnabled = true; settings.clickEffectsEnabled = true;
    settings.highlightStyle = choice == 2 ? kHighlightRing : choice == 1 ? kHighlightGlow : kHighlightFilled;
    settings.highlightRadius = choice == 0 ? 20 : choice == 1 ? 34 : 42;
    settings.highlightOpacity = choice == 0 ? 22 : choice == 1 ? 45 : 80;
    settings.highlightThickness = choice == 2 ? 4 : 3;
    settings.clickStyle = choice == 0 ? kClickRing : choice == 1 ? kClickRipple : kClickNeon;
    settings.clickRadius = choice == 0 ? 32 : choice == 1 ? 48 : 64;
    settings.clickOpacity = choice == 0 ? 65 : 90; settings.clickDurationMs = choice == 0 ? 250 : choice == 1 ? 420 : 560;
    settings.clickThickness = choice == 2 ? 5 : 3; settings.clickEasing = 1;
    SyncControls(); Persist(false); Layout();
}

void App::BrowseSound(unsigned index) {
    if (index >= kSounds.size()) return;
    const auto path = PickSound();
    if (!path.empty()) { settings.*kSounds[index].member = path; Persist(false); }
    SyncControls();
}

void App::SoundPresetChanged(int id) {
    if (syncing) return;
    for (unsigned n = 0; n < kSounds.size(); ++n) if (id == kSounds[n].preset) {
        const LRESULT selected = SendMessageW(controls[id], CB_GETCURSEL, 0, 0);
        if (selected > 0) {
            // Dropdown rows map to the filtered subset for this button.
            LRESULT index = 0;
            for (const auto& choice : soundChoices) {
                if (!(choice.buttons & kSounds[n].mask)) continue;
                if (++index == selected) { settings.*kSounds[n].member = choice.path; break; }
            }
            SyncControls(); Persist(false);
        } else if (selected == 0) BrowseSound(n);
        return;
    }
}

void App::KeyboardSetChanged() {
    if (syncing) return;
    const LRESULT selected = SendMessageW(controls[KeyboardSet], CB_GETCURSEL, 0, 0);
    if (selected > 0 && static_cast<std::size_t>(selected) <= keyboardSets.size()) {
        settings.keyboardSoundPath = keyboardSets[static_cast<std::size_t>(selected) - 1].path;
        SyncControls(); Persist(false);
    } else if (selected == 0) {
        const auto path = PickFolder();
        if (!path.empty()) { settings.keyboardSoundPath = path; Persist(false); }
        SyncControls();
    }
}

void App::ClearSound(unsigned index) {
    if (index >= kSounds.size()) return;
    (settings.*kSounds[index].member).clear();
    SyncControls(); Persist(false);
}

void App::BrowseExcludedApp(bool allowed) {
    const auto path = PickFile(0);
    if (path.empty()) return;
    auto& processes = allowed ? settings.allowedProcesses : settings.excludedProcesses;
    const int id = allowed ? Allowed : Excluded;
    processes = ControlText(controls[id]);
    const auto name = PathLeaf(path);
    if (!processes.empty() && processes.back() != L';') processes += L"; ";
    processes += name;
    SetWindowTextW(controls[id], processes.c_str());
    Persist(false);
}

bool App::PickColor(COLORREF& value) {
    CHOOSECOLORW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
    dialog.rgbResult = value; dialog.lpCustColors = customColors.data();
    dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&dialog)) return false;
    value = dialog.rgbResult; Persist(false);
    return true;
}

void App::ApplyExclusions(bool allowed) {
    (allowed ? settings.allowedProcesses : settings.excludedProcesses) = ControlText(controls[allowed ? Allowed : Excluded]);
    Persist(false);
}

void App::ReadStatus() {
    if (started) status = runtime.Snapshot();
    for (const auto& sound : kSounds) EnableWindow(controls[sound.test], started && !(settings.*sound.member).empty());
    EnableWindow(controls[KeyboardTest], started && !settings.keyboardSoundPath.empty());
    if (status.previewRevision != previewRevision) {
        HCURSOR first = runtime.CopyPreview(false), second = runtime.CopyPreview(true);
        if (normalPreview) DestroyCursor(normalPreview);
        if (darkPreview) DestroyCursor(darkPreview);
        normalPreview = first; darkPreview = second;
        normalPreviewSize = PreviewSize(first); darkPreviewSize = PreviewSize(second);
        previewRevision = status.previewRevision;
    }
    UpdateTray();
    if (IsWindowVisible(window)) {
        RECT client{}; GetClientRect(window, &client);
        const RECT sidebar{0, 0, S(sidebarWidth), client.bottom - S(kFooterHeight)};
        const RECT header{S(sidebarWidth), 0, client.right, S(kHeaderHeight)};
        InvalidateRect(window, &sidebar, FALSE);
        InvalidateRect(window, &header, FALSE);
    }
}

bool App::StartEngine() {
    // A timed-out Start may still have an initializing worker. Finish its
    // restoration before the guard is ever told this is a clean shutdown.
    runtime.Stop(); started = false;
    guard.StopCleanly();
    startupError.clear();
    try {
        if (guard.Start(startupError))
            started = runtime.Start(window, kStatusChanged, settings, startupError);
    } catch (...) {
        startupError = L"The cursor services could not be started. Choose Retry engine to try again.";
    }
    if (!started) {
        runtime.Stop();
        guard.StopCleanly();
    }
    for (int id : {Enable, DarkCursor, Left, Right, Middle, Side1, Side2, PauseFullscreen,
                   PauseConfined, HighlightEnable, TrailEnable, ClicksEnable, SoundEnable, KeyboardEnable,
                   MagnifierEnable, KeyDisplayEnable, KeyDisplayTest})
        EnableWindow(controls[id], started);
    // Keep retry available after a guard, Raw Input, or event subscription
    // failure. A missing profile keeps a healthy worker alive as before.
    EnableWindow(controls[Refresh], TRUE);
    SetWindowTextW(controls[Refresh], started ? L"Reload cursors" : L"Retry engine");
    SyncControls(); ReadStatus();
    return started;
}

void App::AddTray() {
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage; data.hIcon = smallIcon ? smallIcon : icon;
    lstrcpynW(data.szTip, L"ClickShade - click to open settings", 128);
    trayLabel.clear();
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

void App::UpdateTray() {
    if (!window) return;
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1; data.uFlags = NIF_TIP;
    const std::wstring label = settings.enabled ? L"ClickShade - enabled" : L"ClickShade - paused";
    if (trayLabel == label) return;
    trayLabel = label;
    lstrcpynW(data.szTip, label.c_str(), 128);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::Show() {
    ShowWindow(window, SW_SHOWNORMAL); SetForegroundWindow(window);
    ReadStatus();
    StartSpectrum();
}

void App::TrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, TrayOpen, L"Open ClickShade");
    AppendMenuW(menu, MF_STRING | (settings.enabled ? MF_CHECKED : 0), TrayEnable, L"Enable ClickShade");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (settings.highlightEnabled ? MF_CHECKED : 0), TrayHighlight, L"Cursor highlight");
    AppendMenuW(menu, MF_STRING | (settings.clickEffectsEnabled ? MF_CHECKED : 0), TrayClicks, L"Click effects");
    AppendMenuW(menu, MF_STRING | (settings.clickSoundsEnabled ? MF_CHECKED : 0), TraySounds, L"Click sounds");
    AppendMenuW(menu, MF_STRING | (settings.keyboardSoundsEnabled ? MF_CHECKED : 0), TrayKeyboard, L"Keyboard sounds");
    AppendMenuW(menu, MF_STRING | (settings.magnifierEnabled ? MF_CHECKED : 0), TrayMagnifier, L"Magnifier");
    AppendMenuW(menu, MF_STRING | (settings.keyDisplayEnabled ? MF_CHECKED : 0), TrayKeyDisplay, L"Key display");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TrayRestore, L"Restore cursors and pause");
    AppendMenuW(menu, MF_STRING, TrayQuit, L"Quit");
    POINT point{}; GetCursorPos(&point); SetForegroundWindow(window);
    const UINT result = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (result) Command(result);
}

void App::RestoreAndPause() {
    settings.enabled = false; SyncControls(); Persist(false);
    if (started) runtime.RefreshCursors();
    else CursorEngine::RestoreSystemScheme();
    ReadStatus();
}

std::wstring App::PickProfile(bool save) {
    std::vector<wchar_t> path(32768, 0);
    if (save) lstrcpynW(path.data(), L"ClickShade-profile.ini", static_cast<int>(path.size()));
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
    dialog.lpstrFilter = L"ClickShade Windows profile (*.ini)\0*.ini\0\0";
    dialog.lpstrFile = path.data(); dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrDefExt = L"ini";
    dialog.lpstrTitle = save ? L"Export ClickShade profile" : L"Import ClickShade profile";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_DONTADDTORECENT | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) return path.data();
    if (const DWORD code = CommDlgExtendedError()) {
        const auto error = L"The profile picker could not open (Windows error " + std::to_wstring(code) + L").";
        MessageBoxW(window, error.c_str(), L"ClickShade", MB_ICONWARNING);
    }
    return {};
}

void App::ExportSettingsProfile(bool backup) {
    const auto path = backup ? SettingsProfileBackupPath(false) : PickProfile(true);
    if (path.empty()) {
        if (backup) MessageBoxW(window, L"Your profile backup folder could not be opened.", L"ClickShade", MB_ICONWARNING);
        return;
    }
    Settings snapshot = settings;
    snapshot.startWithWindows = IsStartupEnabled();
    std::wstring error;
    if (!SaveSettingsProfile(path, snapshot, error)) {
        MessageBoxW(window, error.c_str(), L"Profile was not saved", MB_ICONWARNING); return;
    }
    const auto text = (backup ? L"Current preferences backed up to:\n\n" : L"Profile exported to:\n\n") + path +
        L"\n\nUse Import profile to restore these preferences. Sound and cursor files are linked, not copied.";
    MessageBoxW(window, text.c_str(), L"ClickShade", MB_OK | MB_ICONINFORMATION);
}

bool App::ApplyImportedSettings(Settings candidate, std::wstring& error) {
    error.clear();
    CancelShortcutCapture();
    // Startup registration and administrator mode belong to this PC. A
    // profile records their old values for reference only.
    candidate.startWithWindows = IsStartupEnabled();
    candidate.runAsAdministrator = settings.runAsAdministrator;
    Settings previous = settings; previous.startWithWindows = candidate.startWithWindows;
    const auto backup = SettingsProfileBackupPath(true);
    if (backup.empty()) { error = L"The previous-settings backup folder could not be opened. No preferences were changed."; return false; }
    if (!SaveSettingsProfile(backup, previous, error)) {
        error = L"The previous settings could not be backed up. No preferences were changed.\n\n" + error; return false;
    }
    if (!RegisterShortcuts(candidate, error)) {
        std::wstring recovery;
        RegisterShortcuts(previous, recovery);
        if (!recovery.empty()) error += L"\n\nPrevious shortcut recovery: " + recovery;
        return false;
    }
    if (!SaveSettings(candidate, error)) {
        std::wstring recovery;
        RegisterShortcuts(previous, recovery);
        if (!recovery.empty()) error += L"\n\nPrevious shortcut recovery: " + recovery;
        return false;
    }
    KillTimer(window, kSaveTimer); pendingSave = false;
    settings = std::move(candidate); pressedAutomatically = false; hotkeyDraftDirty = false;
    shortcutError = false; shortcutStatus = L"Profile shortcuts applied.";
    customColors.fill(0);
    if (started) runtime.UpdateSettings(settings);
    SyncControls();
    SetWindowTextW(controls[Excluded], settings.excludedProcesses.c_str());
    SetWindowTextW(controls[Allowed], settings.allowedProcesses.c_str());
    ApplyTheme(); Scale(true); Layout(); ReadStatus(); UpdateTray();
    return true;
}

void App::ImportSettingsProfile() {
    const auto path = PickProfile(false); if (path.empty()) return;
    Settings candidate; std::wstring error;
    if (!LoadSettingsProfile(path, candidate, error)) {
        MessageBoxW(window, error.c_str(), L"Profile was not imported", MB_ICONWARNING); return;
    }
    const int answer = MessageBoxW(window,
        L"Apply all preferences from this Windows profile?\n\n"
        L"Your current preferences will first replace the single before-import backup. "
        L"Startup registration and administrator mode stay unchanged. Sound and cursor paths may need to be selected again on this PC.",
        L"Import profile?", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (answer != IDOK) return;
    if (!ApplyImportedSettings(std::move(candidate), error)) {
        MessageBoxW(window, (L"The profile was not applied.\n\n" + error).c_str(), L"ClickShade", MB_ICONWARNING); return;
    }
    const auto message = L"Profile imported. Your previous preferences are recoverable with Import profile from:\n\n" + SettingsProfileBackupPath(true);
    MessageBoxW(window, message.c_str(), L"ClickShade", MB_OK | MB_ICONINFORMATION);
}

void App::ResetAllSettings() {
    CancelShortcutCapture();
    const int answer = MessageBoxW(window,
        L"Restore every ClickShade option to its original default?\n\n"
        L"This resets cursor profiles, sizes, click behavior, highlights, trails, sounds, keyboard sounds, the magnifier, the key display, "
        L"rules, colors, shortcuts, display scaling, administrator mode, startup and tray preferences. ClickShade will no longer start with Windows.\n\n"
        L"Selected sound and cursor files stay on your computer; only their selections are cleared.",
        L"Restore all defaults?", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (answer != IDOK) return;
    const Settings defaults{};
    std::wstring error;
    // Save the complete replacement before changing the live runtime. An
    // unsuccessful atomic save leaves the old preferences in place.
    if (!SaveSettings(defaults, error)) {
        MessageBoxW(window, (L"Defaults were not applied.\n\n" + error).c_str(), L"ClickShade", MB_ICONWARNING);
        return;
    }
    if (!ConfigureStartup(false, false, error)) {
        // Startup is a separate OS preference. Restore the old INI if its
        // removal fails, and leave live effects/settings untouched.
        std::wstring rollbackError;
        if (!SaveSettings(settings, rollbackError)) {
            pendingSave = true;
            error += L"\n\nThe previous preferences remain active, but could not be saved again: " + rollbackError;
        }
        MessageBoxW(window, (L"Defaults were not applied.\n\n" + error).c_str(), L"ClickShade", MB_ICONWARNING);
        return;
    }
    KillTimer(window, kSaveTimer);
    pendingSave = false;
    settings = defaults;
    pressedAutomatically = false; hotkeyDraftDirty = false;
    customColors.fill(0);
    SetupShortcuts();
    if (started) runtime.UpdateSettings(settings);
    SyncControls();
    SetWindowTextW(controls[Excluded], L"");
    SetWindowTextW(controls[Allowed], L"");
    ApplyTheme();
    Scale(true);
    Layout();
    ReadStatus();
}

void App::Exit() {
    if (exiting) return;
    FlushSettings();
    CancelShortcutCapture();
    StopSpectrum();
    exiting = true;
    runtime.Stop(); started = false;
    guard.StopCleanly();
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    UnregisterShortcuts();
    DestroyWindow(window);
}

void App::UnregisterShortcuts() {
    for (unsigned n = 0; n < registeredHotkeys.size(); ++n) {
        if (registeredHotkeys[n]) UnregisterHotKey(window, kHotkeyBase + n);
        registeredHotkeys[n] = false;
    }
    hotkeyAvailable = false;
}

bool App::RegisterShortcuts(const Settings& source, std::wstring& error) {
    UnregisterShortcuts(); error.clear();
    for (unsigned n = 0; n < kHotkeys.size(); ++n) {
        HotkeyChord chord; std::wstring canonical, detail;
        if (!ParseHotkey(source.*kHotkeys[n].member, chord, canonical, detail)) {
            if (error.empty()) error = std::wstring(kHotkeys[n].label) + L": " + detail;
            continue;
        }
        if (!chord.key) continue;
        if (RegisterHotKey(window, kHotkeyBase + n, chord.modifiers | MOD_NOREPEAT, chord.key))
            registeredHotkeys[n] = true;
        else if (error.empty()) {
            const DWORD code = GetLastError();
            error = canonical + L" is unavailable. Choose a different shortcut (Windows error " + std::to_wstring(code) + L").";
        }
    }
    hotkeyAvailable = registeredHotkeys[kHotkeys.size() - 1];
    return error.empty();
}

void App::SetupShortcuts() {
    shortcutError = !RegisterShortcuts(settings, shortcutStatus);
    if (!shortcutError) shortcutStatus = L"Shortcuts work while ClickShade is running, including in the tray.";
}

void App::CancelShortcutCapture() {
    if (capturingShortcut < 0) return;
    const int row = capturingShortcut; capturingShortcut = -1;
    suppressShortcutCharacters = false;
    syncing = true; SetWindowTextW(controls[HotkeyFieldBase + row], captureOriginal.c_str()); syncing = false;
    SetWindowTextW(controls[HotkeyRecordBase + row], L"Record");
    SetupShortcuts(); Layout();
}

void App::BeginShortcutCapture(unsigned row) {
    if (capturingShortcut == static_cast<int>(row)) { CancelShortcutCapture(); return; }
    CancelShortcutCapture();
    captureOriginal = ControlText(controls[HotkeyFieldBase + row]);
    capturingShortcut = static_cast<int>(row); suppressShortcutCharacters = true;
    UnregisterShortcuts();
    SetWindowTextW(controls[HotkeyRecordBase + row], L"Cancel");
    shortcutError = false; shortcutStatus = L"Press Ctrl, Alt, or Win with a key. Press Esc to cancel recording.";
    SetFocus(controls[HotkeyFieldBase + row]);
    SendMessageW(controls[HotkeyFieldBase + row], EM_SETSEL, 0, -1);
    Layout();
}

void App::CaptureShortcut(UINT key, UINT modifiers) {
    if (capturingShortcut < 0) return;
    if (key == VK_ESCAPE && !modifiers) { CancelShortcutCapture(); suppressShortcutCharacters = true; return; }
    if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_MENU || key == VK_LMENU ||
        key == VK_RMENU || key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT || key == VK_LWIN || key == VK_RWIN) return;
    HotkeyChord chord; std::wstring canonical, error;
    if (!ParseHotkey(FormatHotkey({modifiers, key}), chord, canonical, error) || canonical.empty()) {
        shortcutError = true; shortcutStatus = error.empty() ? L"Choose a letter, number, function or navigation key with Ctrl, Alt or Win." : error;
        Layout(); return;
    }
    const int row = capturingShortcut; capturingShortcut = -1;
    SetWindowTextW(controls[HotkeyRecordBase + row], L"Record");
    SetWindowTextW(controls[HotkeyFieldBase + row], canonical.c_str()); hotkeyDraftDirty = true;
    SetupShortcuts();
    if (!shortcutError) shortcutStatus = L"Recorded. Choose Apply shortcuts to use your changes.";
    Layout();
}

void App::ApplyShortcuts() {
    CancelShortcutCapture();
    Settings candidate = settings;
    std::array<HotkeyChord, kHotkeys.size()> chords{};
    for (unsigned n = 0; n < kHotkeys.size(); ++n) {
        std::wstring canonical, error;
        if (!ParseHotkey(ControlText(controls[HotkeyFieldBase + n]), chords[n], canonical, error)) {
            shortcutStatus = std::wstring(kHotkeys[n].label) + L": " + error; shortcutError = true;
            SetFocus(controls[HotkeyFieldBase + n]); Layout(); return;
        }
        for (unsigned p = 0; p < n; ++p) if (chords[n].key && chords[p].key == chords[n].key && chords[p].modifiers == chords[n].modifiers) {
            shortcutStatus = canonical + L" is assigned to two actions. Each action needs a different shortcut.";
            shortcutError = true; SetFocus(controls[HotkeyFieldBase + n]); Layout(); return;
        }
        candidate.*kHotkeys[n].member = canonical;
    }
    std::wstring error;
    if (!RegisterShortcuts(candidate, error)) {
        std::wstring recovery;
        RegisterShortcuts(settings, recovery);
        shortcutStatus = error + L" Your previous assignments were kept.";
        if (!recovery.empty()) shortcutStatus += L" " + recovery;
        shortcutError = true; Layout(); return;
    }
    for (const auto& spec : kHotkeys) settings.*spec.member = candidate.*spec.member;
    hotkeyDraftDirty = false; shortcutError = false; shortcutStatus = L"Shortcuts applied.";
    SyncControls(); Persist(false); Layout();
}

void App::HotkeyAction(unsigned action) {
    if (capturingShortcut >= 0 || action >= kHotkeys.size() || !registeredHotkeys[action]) return;
    switch (action) {
    case 0: Command(TrayEnable); break;
    case 1: Command(TrayHighlight); break;
    case 2: Command(TrayClicks); break;
    case 3: Command(TraySounds); break;
    case 4: Command(TrayKeyboard); break;
    case 5: Command(TrayMagnifier); break;
    case 6: Command(TrayKeyDisplay); break;
    case 7: Show(); break;
    case 8: RestoreAndPause(); break;
    }
}

void App::OpenFolder(const std::wstring& path) {
    if (path.empty()) { MessageBoxW(window, L"That folder is not available.", L"ClickShade", MB_ICONINFORMATION); return; }
    ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::ReconcileStartupRegistration() {
    // The elevated instance owns the scheduled task; migrate silently so the
    // user's single "Start with Windows" choice keeps matching their mode.
    if (!elevated) return;
    std::wstring error;
    if (settings.startWithWindows && settings.runAsAdministrator && !IsStartupElevated())
        ConfigureStartup(true, true, error);
    else if (settings.startWithWindows && !settings.runAsAdministrator && IsStartupElevated())
        ConfigureStartup(true, false, error);
    if (!error.empty()) compatibilityNote = error;
    settings.startWithWindows = IsStartupEnabled();
}

void App::ToggleStartup(bool requested) {
    std::wstring error;
    const bool elevatedTask = settings.runAsAdministrator && elevated;
    if (ConfigureStartup(requested, elevatedTask, error)) {
        settings.startWithWindows = requested;
        compatibilityNote = requested && settings.runAsAdministrator && !elevated
            ? L"Startup was registered without administrator rights. Once ClickShade runs as administrator it will switch the entry to a silent elevated task."
            : L"";
    } else {
        MessageBoxW(window, error.c_str(), L"ClickShade", MB_ICONWARNING);
        settings.startWithWindows = IsStartupEnabled();
    }
    Checked(Startup, settings.startWithWindows); Persist(); Layout();
}

void App::ToggleRunAsAdministrator(bool requested) {
    settings.runAsAdministrator = requested;
    Persist(false);
    FlushSettings();
    compatibilityNote.clear();
    if (requested && !elevated) {
        const int answer = MessageBoxW(window,
            L"ClickShade will restart with administrator rights now. Windows shows one prompt per manual start; "
            L"with Start with Windows it starts silently through a scheduled task.\n\nRestart as administrator?",
            L"Run as administrator", MB_OKCANCEL | MB_ICONQUESTION);
        std::wstring error;
        if (answer == IDOK && RelaunchElevated(error)) { Exit(); return; }
        settings.runAsAdministrator = false;
        pendingSave = true; FlushSettings();
        if (!error.empty()) compatibilityNote = error;
    } else if (!requested && elevated) {
        std::wstring error;
        if (settings.startWithWindows && IsStartupElevated()) ConfigureStartup(true, false, error);
        compatibilityNote = error.empty() ? L"Administrator mode turns off the next time ClickShade starts." : error;
    }
    SyncControls(); Layout();
}

void App::Command(UINT id) {
    if (syncing) return;
    if (id >= static_cast<UINT>(kCardResetBase) && id < static_cast<UINT>(kCardResetBase + kCardResetCount)) { ResetCard(static_cast<int>(id - kCardResetBase)); return; }
    if (id >= static_cast<UINT>(kResetOffset) && id < static_cast<UINT>(kResetOffset + 1000) && Slider(static_cast<int>(id) - kResetOffset)) { ResetSlider(static_cast<int>(id) - kResetOffset); return; }
    if (id >= ShakeOff && id <= ShakeMac) { settings.shakeStyle = static_cast<int>(id - ShakeOff); SyncControls(); Persist(false); Layout(); return; }
    if (id >= HotkeyRecordBase && id < HotkeyRecordBase + kHotkeys.size()) { BeginShortcutCapture(id - HotkeyRecordBase); return; }
    if (id >= HotkeyClearBase && id < HotkeyClearBase + kHotkeys.size()) {
        CancelShortcutCapture(); SetWindowTextW(controls[HotkeyFieldBase + id - HotkeyClearBase], L"");
        hotkeyDraftDirty = true; shortcutError = false; shortcutStatus = L"Choose Apply shortcuts to save your changes.";
        Layout(); return;
    }
    for (unsigned n = 0; n < kPages.size(); ++n) if (id == static_cast<UINT>(kPages[n].id)) {
        CancelShortcutCapture();
        page = static_cast<int>(n); scroll = 0; Layout(); ReadStatus(); return;
    }
    if (id >= CursorCurrent && id <= CursorCustom) { settings.cursorStyle = static_cast<int>(id - CursorCurrent); SyncControls(); Persist(false); Layout(); return; }
    if (id >= EaseLinear && id <= EaseSmooth) { settings.clickEasing = static_cast<int>(id - EaseLinear); Persist(false); Layout(); return; }
    if (id >= HighlightFilledStyle && id <= HighlightSpotlightStyle) { settings.highlightStyle = static_cast<int>(id - HighlightFilledStyle); SyncControls(); Persist(false); Layout(); return; }
    if (id >= ClickRingStyle && id <= ClickTargetStyle) { settings.clickStyle = static_cast<int>(id - ClickRingStyle); Persist(false); Layout(); return; }
    if (id >= PresetSubtle && id <= PresetBold) { EffectPreset(static_cast<int>(id - PresetSubtle)); return; }
    if (id >= TrailDots && id <= TrailFineSparkle) { settings.trailStyle = static_cast<int>(id - TrailDots); Persist(false); Layout(); return; }
    if (id == MagnifierCircle || id == MagnifierSquare) { settings.magnifierShape = id == MagnifierSquare ? 1 : 0; Persist(false); Layout(); return; }
    if (id == MagnifierCentered || id == MagnifierBeside) { settings.magnifierPosition = id == MagnifierBeside ? 1 : 0; Persist(false); Layout(); return; }
    if (id == KeyDisplayShortcuts || id == KeyDisplayAll) { settings.keyDisplayMode = id == KeyDisplayAll ? 1 : 0; Persist(false); Layout(); return; }
    if (id >= KeyDisplayBottom && id <= KeyDisplayBottomRight) { settings.keyDisplayPosition = static_cast<int>(id - KeyDisplayBottom); Persist(false); Layout(); return; }
    for (const auto& spec : kToggles) if (id == static_cast<UINT>(spec.id) && spec.id != Startup) { Persist(); return; }
    if ((id >= Left && id <= Side2)) { Persist(); return; }
    switch (id) {
    case Startup: ToggleStartup(Checked(Startup)); break;
    case RunAsAdmin: ToggleRunAsAdministrator(Checked(RunAsAdmin)); break;
    case RestoreDefaults: ResetAllSettings(); break;
    case ExportProfile: ExportSettingsProfile(false); break;
    case BackupProfile: ExportSettingsProfile(true); break;
    case ImportProfile: ImportSettingsProfile(); break;
    case OpenSettingsFolder: OpenFolder(SettingsDirectory()); break;
    case OpenCrashFolder: OpenFolder(CrashReportDirectory()); break;
    case OpenHomepage: ShellExecuteW(window, L"open", kClickShadeHomepage, nullptr, nullptr, SW_SHOWNORMAL); break;
    case HotkeyApply: ApplyShortcuts(); break;
    case RefreshAuto: settings.visualRefreshRate = Checked(RefreshAuto) ? 0 : 144; SyncControls(); Persist(false); break;
    case IdleHideEnable: settings.highlightIdleHideMs = Checked(IdleHideEnable) ? 2000 : 0; SyncControls(); Persist(false); break;
    case UiScaleAuto: settings.uiScalePercent = Checked(UiScaleAuto) ? 0 : 100; SyncControls(); Persist(false); Scale(true); break;
    case BrowseReleased: BrowseCursor(false); break;
    case BrowsePressed: BrowseCursor(true); break;
    case ClearPressed: settings.pressedCursorPath.clear(); pressedAutomatically = false; SyncControls(); Persist(false); break;
    case ApplyExcluded: ApplyExclusions(false); break;
    case ExcludeBrowse: BrowseExcludedApp(false); break;
    case ApplyAllowed: ApplyExclusions(true); break;
    case AllowBrowse: BrowseExcludedApp(true); break;
    case ClearAllowed: settings.allowedProcesses.clear(); SetWindowTextW(controls[Allowed], L""); Persist(false); break;
    case ThemeMint: Theme(RGB(17, 21, 28), RGB(120, 226, 194)); break;
    case ThemeBlue: Theme(RGB(16, 24, 39), RGB(107, 176, 255)); break;
    case ThemeRose: Theme(RGB(30, 22, 32), RGB(241, 151, 190)); break;
    case ThemeLight: Theme(RGB(241, 244, 249), RGB(54, 113, 215), false); break;
    case ThemeDarkMode: SetThemeMode(true); break;
    case ThemeLightMode: SetThemeMode(false); break;
    case ThemeBackground: PickColor(settings.uiBackground); ApplyTheme(); Layout(); break;
    case ThemeAccent: PickColor(settings.uiAccent); ApplyTheme(); Layout(); break;
    case HighlightColor: PickColor(settings.highlightColor); break;
    case TrailColor: PickColor(settings.trailColor); break;
    case ColorLeft: PickColor(settings.leftClickColor); break;
    case ColorRight: PickColor(settings.rightClickColor); break;
    case ColorMiddle: PickColor(settings.middleClickColor); break;
    case MagnifierBorderColor: PickColor(settings.magnifierBorderColor); break;
    case CursorFillColor: case CursorOutlineColor:
        // Choosing a color is a clear wish to see it: switch the colors on.
        if (PickColor(id == CursorFillColor ? settings.cursorFillColor : settings.cursorOutlineColor) && !settings.cursorColors) {
            settings.cursorColors = true; SyncControls(); Persist(false);
        }
        break;
    case KeyDisplayColor: PickColor(settings.keyDisplayColor); break;
    case KeyDisplayTest: if (started) runtime.PreviewKeyDisplay(); break;
    case BrowseLeft: BrowseSound(0); break;
    case BrowseRight: BrowseSound(1); break;
    case BrowseMiddle: BrowseSound(2); break;
    case ClearLeft: ClearSound(0); break;
    case ClearRight: ClearSound(1); break;
    case ClearMiddle: ClearSound(2); break;
    case TestLeft: case TestRight: case TestMiddle:
        if (started) for (const auto& sound : kSounds) if (id == static_cast<UINT>(sound.test)) runtime.PreviewSound(sound.mask);
        break;
    case KeyboardBrowseFolder: {
        const auto path = PickFolder();
        if (!path.empty()) { settings.keyboardSoundPath = path; Persist(false); }
        SyncControls(); break;
    }
    case KeyboardBrowseFile: {
        const auto path = PickSound();
        if (!path.empty()) { settings.keyboardSoundPath = path; Persist(false); }
        SyncControls(); break;
    }
    case KeyboardTest: if (started) runtime.PreviewKeyboardSound(); break;
    case KeyboardClear: settings.keyboardSoundPath.clear(); SyncControls(); Persist(false); break;
    case RefreshSoundLibrary: RefreshSounds(); break;
    case Hide: CancelShortcutCapture(); FlushSettings(); ShowWindow(window, SW_HIDE); StopSpectrum(); break;
    case Refresh: if (started) runtime.RefreshCursors(); else StartEngine(); break;
    case Quit: case TrayQuit: Exit(); break;
    case TrayOpen: Show(); break;
    case TrayEnable: settings.enabled = !settings.enabled; SyncControls(); Persist(false); break;
    case TrayHighlight: settings.highlightEnabled = !settings.highlightEnabled; SyncControls(); Persist(false); break;
    case TrayClicks: settings.clickEffectsEnabled = !settings.clickEffectsEnabled; SyncControls(); Persist(false); break;
    case TraySounds: settings.clickSoundsEnabled = !settings.clickSoundsEnabled; SyncControls(); Persist(false); break;
    case TrayKeyboard: settings.keyboardSoundsEnabled = !settings.keyboardSoundsEnabled; SyncControls(); Persist(false); break;
    case TrayMagnifier: settings.magnifierEnabled = !settings.magnifierEnabled; SyncControls(); Persist(false); break;
    case TrayKeyDisplay: settings.keyDisplayEnabled = !settings.keyDisplayEnabled; SyncControls(); Persist(false); break;
    case TrayRestore: RestoreAndPause(); break;
    default: break;
    }
}

} // namespace clickshade::ui
