#pragma once
// Shared declarations for the Windows settings window. The window is split
// into ui_layout.cpp (controls and geometry), ui_paint.cpp (custom drawing),
// ui_actions.cpp (commands, dialogs, profiles, shortcuts, tray) and app.cpp
// (window procedures and the entry point).

#include "runtime.h"
#include "cursor_engine.h"
#include "settings.h"
#include "recovery.h"
#include "ui_scale.h"
#include "hotkeys.h"
#include "crash_guard.h"
#include "version.h"
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace clickshade::ui {

constexpr wchar_t kWindowClass[] = L"ClickShade.Settings.Window";
constexpr wchar_t kPanelClass[] = L"ClickShade.Settings.Panel";
constexpr UINT kStatusChanged = WM_APP + 10;
constexpr UINT kTrayMessage = WM_APP + 11;
constexpr UINT kShowSettings = WM_APP + 12;
constexpr UINT kRestoreAndPause = WM_APP + 13;
constexpr UINT kQuitRequest = WM_APP + 14;
constexpr int kHotkeyBase = 1;
constexpr UINT_PTR kSaveTimer = 19;
constexpr UINT_PTR kSpectrumTimer = 20;
constexpr UINT_PTR kHealthyTimer = 21;
constexpr int kValueOffset = 1000;   // Slider id + offset = its numeric edit box.
constexpr int kResetOffset = 2000;   // Slider id + offset = its reset button.
constexpr int kCardResetBase = 700;  // Pool of per-card Reset buttons.
constexpr int kCardResetCount = 16;
constexpr int kNavIconBase = 760;

// Layout constants in DIP.
constexpr int kSidebarWidth = 184;
constexpr int kSidebarCompactWidth = 60;
constexpr int kHeaderHeight = 66;
constexpr int kFooterHeight = 50;
constexpr int kCardPadding = 16;
constexpr int kCardGap = 12;
constexpr int kContentMargin = 18;
constexpr int kTwoColumnMinimum = 850; // Content width needed for two card columns (two 419 DIP cards).
constexpr int kSidebarNavTop = 76;         // First navigation button.
constexpr int kSidebarNavStep = 40;        // Height of one navigation button row.
constexpr int kSidebarStatusTall = 184;    // Master switch, cursor previews, heading and up to four detail lines.
constexpr int kSidebarStatusShort = 64;    // Master switch and heading only, for short windows.
constexpr int kDefaultWindowWidth = 1100;  // DIP: full sidebar plus two comfortable columns.
constexpr int kDefaultWindowHeight = 720;  // DIP fallback before the pages are measured.

inline COLORREF Mix(COLORREF a, COLORREF b, double amount) noexcept {
    const auto channel = [amount](unsigned x, unsigned y) {
        return static_cast<unsigned>(std::lround(x * (1.0 - amount) + y * amount));
    };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)),
               channel(GetBValue(a), GetBValue(b)));
}

inline double Luminance(COLORREF color) noexcept {
    const auto linear = [](unsigned channel) {
        const double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return .2126 * linear(GetRValue(color)) + .7152 * linear(GetGValue(color)) + .0722 * linear(GetBValue(color));
}

// Hue in degrees, saturation/value 0..1.
inline COLORREF FromHsv(double hue, double saturation, double value) noexcept {
    hue = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0) / 60.0;
    const int sector = static_cast<int>(hue);
    const double fraction = hue - sector;
    const double p = value * (1 - saturation), q = value * (1 - saturation * fraction),
                 t = value * (1 - saturation * (1 - fraction));
    double r = value, g = t, b = p;
    switch (sector) {
    case 1: r = q; g = value; b = p; break;
    case 2: r = p; g = value; b = t; break;
    case 3: r = p; g = q; b = value; break;
    case 4: r = t; g = p; b = value; break;
    case 5: r = value; g = p; b = q; break;
    default: break;
    }
    return RGB(static_cast<int>(r * 255 + .5), static_cast<int>(g * 255 + .5), static_cast<int>(b * 255 + .5));
}

struct Palette {
    COLORREF background{}, sidebar{}, card{}, cardHover{}, border{}, edit{}, text{}, muted{}, accent{}, accentText{}, accentInk{}, warning{}, success{};
    bool dark = true;
    Palette() = default;
    explicit Palette(const Settings& settings) {
        background = settings.uiBackground; accent = settings.uiAccent;
        dark = Luminance(background) < .18;
        const COLORREF contrast = dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
        sidebar = Mix(background, contrast, dark ? .025 : .035);
        card = Mix(background, contrast, dark ? .05 : .03);
        cardHover = Mix(background, contrast, dark ? .08 : .05);
        border = Mix(background, contrast, dark ? .15 : .16);
        edit = Mix(background, contrast, dark ? .085 : .06);
        text = dark ? RGB(234, 239, 247) : RGB(22, 28, 38);
        muted = Mix(card, text, .62);
        accentText = Luminance(accent) > .18 ? RGB(15, 24, 29) : RGB(250, 252, 255);
        accentInk = accent;
        const auto contrastRatio = [](COLORREF a, COLORREF b) {
            const double first = Luminance(a), second = Luminance(b);
            return (std::max(first, second) + .05) / (std::min(first, second) + .05);
        };
        for (unsigned n = 0; n < 8 && contrastRatio(accentInk, card) < 4.5; ++n)
            accentInk = Mix(accentInk, text, .25);
        warning = dark ? RGB(255, 183, 112) : RGB(145, 62, 20);
        success = dark ? RGB(126, 231, 164) : RGB(23, 125, 72);
    }
};

enum Id {
    Enable = 100, Left, Right, Middle, Side1, Side2, PauseFullscreen,
    Excluded, ApplyExcluded, Startup, Minimized, Hide, Refresh, Quit,
    DarkCursor, PauseConfined, ExcludeBrowse,
    PageCursor, PageHighlight, PageClicks, PageSounds, PagePresenter, PageRules, PageAppearance, PageShortcuts,
    HighlightEnable, HighlightColor, HighlightRadius, HighlightOpacity, HighlightThickness,
    HighlightFilledStyle, HighlightRingStyle, HighlightGlowStyle, HighlightCrosshairStyle, HighlightSpotlightStyle,
    HighlightBreathing, IdleHideEnable, IdleHideDelay, SpotlightRadius, SpotlightDim,
    ClicksEnable, ClickLeft, ClickRight, ClickMiddle, ColorLeft, ColorRight, ColorMiddle,
    ClickRingStyle, ClickPulseStyle, ClickRippleStyle, ClickBurstStyle, ClickNeonStyle, ClickTargetStyle,
    ClickRadius, ClickDuration, ClickOpacity, ClickThickness, EaseLinear, EaseCubic, EaseSmooth,
    RefreshRate, RefreshAuto,
    SoundEnable, SoundVolume, RefreshSoundLibrary,
    LeftSound, RightSound, MiddleSound, LeftSoundPreset, RightSoundPreset, MiddleSoundPreset,
    BrowseLeft, BrowseRight, BrowseMiddle, TestLeft, TestRight, TestMiddle, ClearLeft, ClearRight, ClearMiddle,
    KeyboardEnable, KeyboardVolume, KeyboardSet, KeyboardPath, KeyboardBrowseFolder, KeyboardBrowseFile, KeyboardTest, KeyboardClear,
    RunAsAdmin, CaptureVisible,
    ThemeMint, ThemeBlue, ThemeRose, ThemeLight, ThemeBackground, ThemeAccent, ThemeDarkMode, ThemeLightMode,
    UiScale, UiScaleAuto, AnimatedAccent, PresetSubtle, PresetPresenter, PresetBold, RestoreDefaults,
    ExportProfile, BackupProfile, ImportProfile, OpenSettingsFolder, OpenCrashFolder, OpenHomepage,
    TrailEnable, TrailDots, TrailRings, TrailSparkles, TrailComet, TrailFineSparkle,
    TrailRadius, TrailOpacity, TrailDuration, TrailColor,
    Allowed, ApplyAllowed, AllowBrowse, ClearAllowed,
    CursorCurrent, CursorWindows, CursorClassic, CursorCustom,
    CursorReverse, CursorHold, CursorSize, CursorFlash, CursorOpacity, CursorShadow, CursorColorsEnable,
    CursorFillColor, CursorOutlineColor,
    ReleasedFile, PressedFile, BrowseReleased, BrowsePressed, ClearPressed,
    ShakeOff, ShakePlasma, ShakeMac, ShakeSensitivity, ShakeMaximum,
    MagnifierEnable, MagnifierBorderColor, MagnifierCircle, MagnifierSquare, MagnifierCentered, MagnifierBeside, MagnifierFrame,
    MagnifierZoom, MagnifierSize,
    KeyDisplayEnable, KeyDisplayColor, KeyDisplayShortcuts, KeyDisplayAll, KeyDisplayMouse,
    KeyDisplayBottom, KeyDisplayTop, KeyDisplayBottomLeft, KeyDisplayBottomRight,
    KeyDisplaySize, KeyDisplayDuration, KeyDisplayOpacity, KeyDisplayTest,
    HotkeyFieldBase = 500, HotkeyRecordBase = 520, HotkeyClearBase = 540, HotkeyApply = 560,
    TrayOpen = 300, TrayEnable, TrayRestore, TrayQuit, TrayHighlight, TrayClicks, TraySounds, TrayKeyboard,
    TrayMagnifier, TrayKeyDisplay,
};

struct PageSpec { int id; const wchar_t* label; const wchar_t* title; const wchar_t* subtitle; };
constexpr std::array<PageSpec, 8> kPages{{
    {PageCursor, L"Cursor", L"Cursor", L"Pressed appearance, trigger buttons, sizing and shake to locate."},
    {PageHighlight, L"Highlight", L"Highlight & trails", L"A steady halo, spotlight or crosshair, and an optional fading trail."},
    {PageClicks, L"Clicks", L"Click effects", L"Per-button animations with their own colors, shape and timing."},
    {PageSounds, L"Sounds", L"Sounds", L"Mouse click sounds per button and a typing sound set for the keyboard."},
    {PagePresenter, L"Presenter", L"Presenter tools", L"A magnifier lens that follows the pointer and an on-screen display of the keys you press."},
    {PageRules, L"Rules", L"Rules & compatibility", L"When effects pause, how ClickShade starts, and how it behaves around admin apps and recordings."},
    {PageAppearance, L"Appearance", L"Appearance", L"Make the settings window yours, set the frame pacing, keep profiles, or start fresh."},
    {PageShortcuts, L"Shortcuts", L"Global shortcuts", L"Exact key combinations that work while ClickShade runs in the tray."},
}};
constexpr int kPresenterPage = 4;
constexpr int kAppearancePage = 6;
constexpr int kShortcutsPage = 7;

struct HotkeySpec { const wchar_t* label; std::wstring Settings::* member; };
// Restore stays last: its registration decides whether shortcuts are available.
constexpr std::array<HotkeySpec, 9> kHotkeys{{
    {L"Toggle all effects", &Settings::hotkeyToggleAll},
    {L"Toggle highlight", &Settings::hotkeyHighlight},
    {L"Toggle click effects", &Settings::hotkeyClicks},
    {L"Toggle click sounds", &Settings::hotkeySounds},
    {L"Toggle keyboard sounds", &Settings::hotkeyKeyboardSounds},
    {L"Toggle magnifier", &Settings::hotkeyMagnifier},
    {L"Toggle key display", &Settings::hotkeyKeyDisplay},
    {L"Show settings", &Settings::hotkeySettings},
    {L"Restore cursors and pause", &Settings::hotkeyRestore},
}};

struct SliderSpec { int id; int Settings::* member; int minimum; int maximum; const wchar_t* label; const wchar_t* unit; bool live; };
// "live" sliders update while dragging; the others commit when the thumb is
// released because they rebuild cursor artwork or fonts.
constexpr std::array<SliderSpec, 27> kSliders{{
    {HighlightRadius, &Settings::highlightRadius, 8, 100, L"Radius", L"dip", true},
    {HighlightOpacity, &Settings::highlightOpacity, 5, 100, L"Opacity", L"%", true},
    {HighlightThickness, &Settings::highlightThickness, 1, 12, L"Ring width", L"dip", true},
    {SpotlightRadius, &Settings::spotlightRadius, 40, 400, L"Spot radius", L"dip", true},
    {SpotlightDim, &Settings::spotlightDim, 10, 90, L"Dimming", L"%", true},
    {IdleHideDelay, &Settings::highlightIdleHideMs, 500, 10000, L"Hide after", L"ms", true},
    {ClickRadius, &Settings::clickRadius, 12, 120, L"Radius", L"dip", true},
    {ClickDuration, &Settings::clickDurationMs, 100, 1500, L"Duration", L"ms", true},
    {ClickOpacity, &Settings::clickOpacity, 5, 100, L"Opacity", L"%", true},
    {ClickThickness, &Settings::clickThickness, 1, 12, L"Ring width", L"dip", true},
    {SoundVolume, &Settings::soundVolume, 0, 100, L"Volume", L"%", true},
    {KeyboardVolume, &Settings::keyboardVolume, 0, 100, L"Volume", L"%", true},
    {CursorSize, &Settings::cursorScale, 100, 600, L"Cursor scale", L"%", false},
    {CursorOpacity, &Settings::cursorOpacity, 30, 100, L"Opacity", L"%", false},
    {CursorFlash, &Settings::cursorFlashMs, 50, 500, L"Brief press", L"ms", true},
    {RefreshRate, &Settings::visualRefreshRate, 30, 360, L"Frame limit", L"fps", true},
    {UiScale, &Settings::uiScalePercent, 100, 200, L"Extra zoom", L"%", false},
    {TrailRadius, &Settings::trailRadius, 2, 16, L"Stamp radius", L"dip", true},
    {TrailOpacity, &Settings::trailOpacity, 5, 100, L"Opacity", L"%", true},
    {TrailDuration, &Settings::trailDurationMs, 80, 700, L"Fade time", L"ms", true},
    {ShakeSensitivity, &Settings::shakeSensitivity, 50, 200, L"Sensitivity", L"%", true},
    {ShakeMaximum, &Settings::shakeMaximumScale, 150, 400, L"Maximum size", L"%", true},
    {MagnifierZoom, &Settings::magnifierZoom, 125, 800, L"Zoom", L"%", true},
    {MagnifierSize, &Settings::magnifierSize, 80, 600, L"Lens size", L"dip", true},
    {KeyDisplaySize, &Settings::keyDisplaySize, 60, 250, L"Text size", L"%", true},
    {KeyDisplayDuration, &Settings::keyDisplayDurationMs, 300, 6000, L"Hold time", L"ms", true},
    {KeyDisplayOpacity, &Settings::keyDisplayOpacity, 20, 100, L"Opacity", L"%", true},
}};

struct ToggleSpec { int id; bool Settings::* member; };
constexpr std::array<ToggleSpec, 25> kToggles{{
    {Enable, &Settings::enabled}, {DarkCursor, &Settings::darkCursorEnabled},
    {CursorReverse, &Settings::cursorReverse}, {CursorHold, &Settings::cursorHold},
    {PauseFullscreen, &Settings::pauseFullscreen}, {PauseConfined, &Settings::pauseConfinedCursor},
    {Minimized, &Settings::startMinimized}, {CaptureVisible, &Settings::captureVisible},
    {HighlightEnable, &Settings::highlightEnabled}, {HighlightBreathing, &Settings::highlightBreathing},
    {TrailEnable, &Settings::trailEnabled},
    {ClicksEnable, &Settings::clickEffectsEnabled}, {ClickLeft, &Settings::leftClickEnabled},
    {ClickRight, &Settings::rightClickEnabled}, {ClickMiddle, &Settings::middleClickEnabled},
    {SoundEnable, &Settings::clickSoundsEnabled}, {KeyboardEnable, &Settings::keyboardSoundsEnabled},
    {MagnifierEnable, &Settings::magnifierEnabled}, {KeyDisplayEnable, &Settings::keyDisplayEnabled},
    {KeyDisplayMouse, &Settings::keyDisplayMouse}, {MagnifierFrame, &Settings::magnifierFrame},
    {CursorShadow, &Settings::cursorShadow}, {CursorColorsEnable, &Settings::cursorColors},
    {AnimatedAccent, &Settings::uiAnimatedAccent}, {Startup, &Settings::startWithWindows},
}};

struct SoundSpec { int field, preset, browse, test, clear; unsigned mask; const wchar_t* label; const wchar_t* token; std::wstring Settings::* member; };
constexpr std::array<SoundSpec, 3> kSounds{{
    {LeftSound, LeftSoundPreset, BrowseLeft, TestLeft, ClearLeft, 1, L"Left button", L"left", &Settings::leftSoundPath},
    {RightSound, RightSoundPreset, BrowseRight, TestRight, ClearRight, 2, L"Right button", L"right", &Settings::rightSoundPath},
    {MiddleSound, MiddleSoundPreset, BrowseMiddle, TestMiddle, ClearMiddle, 4, L"Middle button", L"middle", &Settings::middleSoundPath},
}};

struct SoundChoice { std::wstring path, label; unsigned buttons = 7; }; // buttons: which dropdowns list it.
struct KeyboardSetChoice { std::wstring path, label; };

bool SameFilePath(std::wstring first, std::wstring second);
std::vector<SoundChoice> FindSoundChoices();
std::vector<KeyboardSetChoice> FindKeyboardSets();
std::wstring PathLeaf(const std::wstring& path);

// Items the layout computes once and the panel paints every frame. All
// coordinates are DIP relative to the scrolled content origin.
struct PaintText { std::wstring text; RECT rect; int font; COLORREF color; UINT flags; };
struct PaintFrame { RECT rect; int radius; bool card; };
enum FontKind { FontNormal = 0, FontSmall, FontSemibold, FontTitle, FontCount };

struct App {
    HWND window = nullptr;
    HWND panel = nullptr;
    Runtime runtime;
    RecoveryGuard guard;
    Settings settings = LoadSettings();
    Palette colors{settings};
    RuntimeStatus status;
    std::map<int, HWND> controls;
    std::map<int, RECT> editFrames; // Panel DIP rects for custom edit borders.
    HBRUSH background = nullptr, sidebarBrush = nullptr, cardBrush = nullptr, editBrush = nullptr;
    std::array<HFONT, FontCount> fonts{};
    HICON icon = nullptr, windowIcon = nullptr, smallIcon = nullptr;
    int iconDpi = 0, iconSystemDpi = 0;
    HCURSOR normalPreview = nullptr, darkPreview = nullptr;
    SIZE normalPreviewSize{}, darkPreviewSize{};
    std::uint64_t previewRevision = 0;
    UINT taskbarCreated = 0;
    int dpi = 96, systemDpi = 96, scroll = 0;
    int contentHeight = 600;      // DIP height of the current page.
    int sidebarWidth = kSidebarWidth;
    int columns = 2;
    HDC paintBuffer = nullptr;
    HBITMAP paintBitmap = nullptr, paintOriginal = nullptr;
    SIZE paintSize{};
    HDC panelBuffer = nullptr;
    HBITMAP panelBitmap = nullptr, panelOriginal = nullptr;
    SIZE panelSize{};
    bool syncing = false, pendingSave = false, scaling = false;
    std::wstring trayLabel;
    int page = 0;
    std::array<COLORREF, 16> customColors{};
    bool started = false, exiting = false, hotkeyAvailable = false, elevated = false;
    bool pressedAutomatically = false, hotkeyDraftDirty = false;
    int capturingShortcut = -1;
    bool suppressShortcutCharacters = false;
    std::wstring captureOriginal, shortcutStatus;
    bool shortcutError = false;
    std::array<bool, kHotkeys.size()> registeredHotkeys{};
    std::wstring startupError;
    std::wstring compatibilityNote;
    CrashNotice crashNotice;
    std::vector<SoundChoice> soundChoices;
    std::vector<KeyboardSetChoice> keyboardSets;
    std::vector<PaintText> paintTexts;
    std::vector<PaintFrame> paintFrames;
    std::vector<std::function<void()>> cardResets;
    std::vector<std::function<bool()>> cardChanged; // True when a card differs from its defaults.
    double spectrumPhase = 0.0;
    ULONGLONG spectrumLast = 0;
    bool spectrumTimerArmed = false;

    ~App();
    int S(int value) const { return MulDiv(value, dpi, 96); }
    int D(int pixels) const { return MulDiv(pixels, 96, dpi); }
    // The sidebar status block shrinks to its heading when the window is too
    // short to keep it clear of the navigation and the footer.
    static int StatusBlockHeight(int heightDip) {
        const int navBottom = kSidebarNavTop + static_cast<int>(kPages.size()) * kSidebarNavStep;
        return heightDip - kFooterHeight - navBottom >= kSidebarStatusTall + 12 ? kSidebarStatusTall : kSidebarStatusShort;
    }

    // ui_layout.cpp
    void LoadIcons();
    void Fonts();
    void DeleteFonts();
    HWND Control(int id, const wchar_t* type, const wchar_t* text, DWORD style);
    void CreateControls();
    void Layout();
    void SyncControls();
    void RefreshSounds();
    void Scale(bool resize);
    void InitialWindowSize();
    void FitWindow(int x, int y, int width, int height);
    RECT WorkArea() const;
    RECT ContentRect() const; // Window client pixels of the scrolling panel.
    void EnsureVisible(HWND child);
    bool ScrollKey(UINT key);
    void ScrollBy(int pixels);
    const SliderSpec* Slider(int id) const;
    bool SliderEnabled(int id) const;
    void Checked(int id, bool value);
    bool Checked(int id) const;
    void FreePaintBuffers();
    HDC Buffer(HDC dc, int width, int height, HDC& buffer, HBITMAP& bitmap, HBITMAP& original, SIZE& size);

    // ui_paint.cpp
    void Paint(HDC dc);
    void PaintPanel(HDC dc);
    void DrawSlider(HWND child, HDC dc);
    void DrawButton(const DRAWITEMSTRUCT* item);
    void DrawToggle(HWND child, HDC dc);
    void DrawSpectrum(HDC dc, const RECT& line);
    void DrawNavGlyph(HDC dc, int pageIndex, int x, int y, int size, COLORREF ink);
    void ApplyTheme();
    void StartSpectrum();
    void StopSpectrum();
    void TickSpectrum();
    std::wstring StatusHeading() const;
    std::wstring StatusDetail() const;
    bool StatusIsWarning() const;

    // ui_actions.cpp
    void Command(UINT id);
    void Persist(bool readButtons = true);
    void FlushSettings();
    void SliderChanged(int id, bool tracking);
    void NumberChanged(int id);
    void ResetSlider(int id);
    void ResetCard(int index);
    std::wstring PickFile(int kind);
    std::wstring PickSound();
    std::wstring PickFolder();
    void SelectCursorFile(const std::wstring& path, bool pressed);
    void BrowseCursor(bool pressed);
    void EffectPreset(int choice);
    void BrowseSound(unsigned index);
    void SoundPresetChanged(int id);
    void KeyboardSetChanged();
    void ClearSound(unsigned index);
    void BrowseExcludedApp(bool allowed);
    bool PickColor(COLORREF& value);
    void ApplyExclusions(bool allowed);
    void ReadStatus();
    bool StartEngine();
    void AddTray();
    void UpdateTray();
    void Show();
    void TrayMenu();
    void RestoreAndPause();
    std::wstring PickProfile(bool save);
    void ExportSettingsProfile(bool backup);
    bool ApplyImportedSettings(Settings candidate, std::wstring& error);
    void ImportSettingsProfile();
    void ResetAllSettings();
    void Exit();
    void UnregisterShortcuts();
    bool RegisterShortcuts(const Settings& source, std::wstring& error);
    void SetupShortcuts();
    void CancelShortcutCapture();
    void BeginShortcutCapture(unsigned row);
    void CaptureShortcut(UINT key, UINT modifiers);
    void ApplyShortcuts();
    void HotkeyAction(unsigned action);
    void ToggleRunAsAdministrator(bool requested);
    void ToggleStartup(bool requested);
    void ReconcileStartupRegistration();
    void OpenFolder(const std::wstring& path);
    void RememberPalette();
    void SetThemeMode(bool dark);
    void Theme(COLORREF base, COLORREF accent, bool dark = true);
    static std::wstring ControlText(HWND control);
    static SIZE PreviewSize(HCURSOR cursor);
};

LRESULT CALLBACK ToggleProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK SliderProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK FocusProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK ShortcutProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK WheelProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

} // namespace clickshade::ui
