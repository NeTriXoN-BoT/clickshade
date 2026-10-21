#include "ui_common.h"
#include <shellapi.h>
#include <uxtheme.h>
#include <cwctype>

namespace clickshade::ui {
namespace {
int CALLBACK FoundFont(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM parameter) {
    *reinterpret_cast<bool*>(parameter) = true;
    return 0;
}

// Places controls and records paint items for one card. Coordinates are DIP
// inside the scrolling panel; the card grows as rows are added.
class CardBuilder {
public:
    CardBuilder(App& app, int x, int y, int width, const wchar_t* title,
                std::map<int, bool>& visible, int resetIndex)
        : app_(app), x_(x), y_(y), width_(width), visible_(visible), resetIndex_(resetIndex) {
        cursor_ = y_ + kCardPadding;
        app_.paintTexts.push_back({title, {x_ + kCardPadding, cursor_ + 1, x_ + width_ - kCardPadding - 70, cursor_ + 25},
                                   FontSemibold, app_.colors.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX});
        if (resetIndex_ >= 0 && resetIndex_ < kCardResetCount)
            Put(kCardResetBase + resetIndex_, x_ + width_ - kCardPadding - 62, cursor_ - 3, 62, 26);
        cursor_ += 34;
    }
    int Inner() const { return width_ - 2 * kCardPadding; }
    int Left() const { return x_ + kCardPadding; }
    void Put(int id, int x, int y, int w, int h) {
        visible_[id] = true;
        if (app_.editFrames.count(id)) {
            app_.editFrames[id] = {x, y, x + w, y + h};
            MoveWindow(app_.controls[id], app_.S(x + 8), app_.S(y + 6) - app_.scroll, app_.S(std::max(8, w - 16)), app_.S(h - 12), FALSE);
        } else {
            MoveWindow(app_.controls[id], app_.S(x), app_.S(y) - app_.scroll, app_.S(std::max(1, w)), app_.S(h), FALSE);
        }
    }
    void Toggle(int id, int height = 30) { Put(id, Left(), cursor_, Inner(), height - 4); cursor_ += height; }
    // Toggle on the left, a color/action button on the right.
    void ToggleWith(int id, int buttonId, int buttonWidth = 132) {
        Put(id, Left(), cursor_ + 2, Inner() - buttonWidth - 10, 26);
        Put(buttonId, Left() + Inner() - buttonWidth, cursor_ - 1, buttonWidth, 32);
        cursor_ += 36;
    }
    // Small toggles in a grid; each cell keeps at least 112 DIP so labels never truncate.
    void CompactToggles(const std::vector<int>& ids) {
        const int perRow = std::clamp(Inner() / 112, 1, static_cast<int>(ids.size()));
        const int step = Inner() / perRow;
        for (std::size_t n = 0; n < ids.size(); ++n) {
            const int row = static_cast<int>(n) / perRow, column = static_cast<int>(n) % perRow;
            Put(ids[n], Left() + column * step, cursor_ + row * 30, step - 6, 24);
        }
        cursor_ += 30 * ((static_cast<int>(ids.size()) + perRow - 1) / perRow);
    }
    void Buttons(const std::vector<int>& ids, int perRow, int height = 34) {
        const int gap = 8;
        const int width = (Inner() - (perRow - 1) * gap) / perRow;
        for (std::size_t n = 0; n < ids.size(); ++n) {
            const int row = static_cast<int>(n) / perRow, column = static_cast<int>(n) % perRow;
            Put(ids[n], Left() + column * (width + gap), cursor_ + row * (height + gap), width, height);
        }
        const int rows = (static_cast<int>(ids.size()) + perRow - 1) / perRow;
        cursor_ += rows * (height + gap) - gap + 10;
    }
    void Slider(int id) {
        const auto* spec = app_.Slider(id);
        if (!spec) return;
        const int labelWidth = 98, valueWidth = 58, unitWidth = 26, resetWidth = 26;
        const int trackLeft = Left() + labelWidth;
        const int trackWidth = Inner() - labelWidth - valueWidth - unitWidth - resetWidth - 16;
        app_.paintTexts.push_back({spec->label, {Left(), cursor_ + 6, Left() + labelWidth - 4, cursor_ + 28}, FontNormal, app_.colors.muted,
                                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX});
        Put(id, trackLeft, cursor_, trackWidth, 30);
        Put(id + kValueOffset, trackLeft + trackWidth + 6, cursor_, valueWidth, 30);
        app_.paintTexts.push_back({spec->unit, {trackLeft + trackWidth + valueWidth + 10, cursor_ + 7, trackLeft + trackWidth + valueWidth + unitWidth + 10, cursor_ + 27},
                                   FontSmall, app_.colors.muted, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX});
        Put(id + kResetOffset, Left() + Inner() - resetWidth, cursor_ + 2, resetWidth, 26);
        cursor_ += 36;
    }
    void Text(const wchar_t* text, int font = FontNormal, int height = 22) {
        app_.paintTexts.push_back({text, {Left(), cursor_, Left() + Inner(), cursor_ + height}, font,
                                   font == FontSmall ? app_.colors.muted : app_.colors.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX});
        cursor_ += height;
    }
    void Hint(const std::wstring& text, COLORREF color = CLR_INVALID) {
        if (text.empty()) return;
        const int height = app_.D(MeasureHeight(text, Inner())) + 2;
        app_.paintTexts.push_back({text, {Left(), cursor_, Left() + Inner(), cursor_ + height}, FontSmall,
                                   color == CLR_INVALID ? app_.colors.muted : color, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX});
        cursor_ += height + 8;
    }
    // Read-only path/text field with trailing buttons.
    void Field(int fieldId, const std::vector<std::pair<int, int>>& buttons /* id, width */) {
        int right = Left() + Inner();
        for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
            Put(it->first, right - it->second, cursor_, it->second, 32);
            right -= it->second + 8;
        }
        Put(fieldId, Left(), cursor_, std::max(40, right - Left()), 32);
        cursor_ += 38;
    }
    void Combo(int id, int height = 30) { Put(id, Left(), cursor_, Inner(), 260); cursor_ += height + 6; }
    int Finish() {
        const int bottom = cursor_ + kCardPadding - 6;
        app_.paintFrames.insert(app_.paintFrames.begin(), PaintFrame{{x_, y_, x_ + width_, bottom}, 14, true});
        return bottom;
    }
private:
    int MeasureHeight(const std::wstring& text, int widthDip) const {
        HDC dc = GetDC(app_.panel);
        if (!dc) return 20;
        HGDIOBJ old = SelectObject(dc, app_.fonts[FontSmall]);
        RECT rect{0, 0, app_.S(widthDip), 0};
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(app_.panel, dc);
        return std::max<int>(rect.bottom, app_.S(16));
    }
    App& app_;
    int x_, y_, width_;
    std::map<int, bool>& visible_;
    int resetIndex_;
    int cursor_;
};
} // namespace

App::~App() {
    runtime.Stop();
    guard.StopCleanly();
    DeleteFonts();
    FreePaintBuffers();
    if (normalPreview) DestroyCursor(normalPreview);
    if (darkPreview) DestroyCursor(darkPreview);
    if (icon) DestroyIcon(icon);
    if (windowIcon) DestroyIcon(windowIcon);
    if (smallIcon) DestroyIcon(smallIcon);
    for (HBRUSH brush : {background, sidebarBrush, cardBrush, editBrush}) if (brush) DeleteObject(brush);
}

void App::LoadIcons() {
    if (iconDpi == dpi && iconSystemDpi == systemDpi && icon && windowIcon && smallIcon) return;
    const auto load = [](int size) {
        HICON result = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
            IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (!result) result = CopyIcon(static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION,
            IMAGE_ICON, size, size, LR_SHARED)));
        return result;
    };
    HICON next = load(S(28));
    HICON nextWindow = load(GetSystemMetricsForDpi(SM_CXICON, systemDpi));
    HICON nextSmall = load(GetSystemMetricsForDpi(SM_CXSMICON, systemDpi));
    if (!next || !nextWindow || !nextSmall) {
        if (next) DestroyIcon(next);
        if (nextWindow) DestroyIcon(nextWindow);
        if (nextSmall) DestroyIcon(nextSmall);
        return;
    }
    if (window) {
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(nextWindow));
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(nextSmall));
        SetClassLongPtrW(window, GCLP_HICON, reinterpret_cast<LONG_PTR>(nextWindow));
        SetClassLongPtrW(window, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(nextSmall));
        NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1;
        data.uFlags = NIF_ICON; data.hIcon = nextSmall; Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    if (icon) DestroyIcon(icon);
    if (windowIcon) DestroyIcon(windowIcon);
    if (smallIcon) DestroyIcon(smallIcon);
    icon = next; windowIcon = nextWindow; smallIcon = nextSmall;
    iconDpi = dpi; iconSystemDpi = systemDpi;
}

void App::FreePaintBuffers() {
    if (paintBuffer && paintOriginal) SelectObject(paintBuffer, paintOriginal);
    if (paintBitmap) DeleteObject(paintBitmap);
    if (paintBuffer) DeleteDC(paintBuffer);
    paintBuffer = nullptr; paintBitmap = paintOriginal = nullptr; paintSize = {};
    if (panelBuffer && panelOriginal) SelectObject(panelBuffer, panelOriginal);
    if (panelBitmap) DeleteObject(panelBitmap);
    if (panelBuffer) DeleteDC(panelBuffer);
    panelBuffer = nullptr; panelBitmap = panelOriginal = nullptr; panelSize = {};
}

HDC App::Buffer(HDC dc, int width, int height, HDC& buffer, HBITMAP& bitmap, HBITMAP& original, SIZE& size) {
    if (buffer && width == size.cx && height == size.cy) return buffer;
    if (buffer && original) SelectObject(buffer, original);
    if (bitmap) DeleteObject(bitmap);
    if (buffer) DeleteDC(buffer);
    buffer = CreateCompatibleDC(dc);
    bitmap = CreateCompatibleBitmap(dc, std::max(1, width), std::max(1, height));
    if (!buffer || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        buffer = nullptr; bitmap = original = nullptr; size = {};
        return nullptr;
    }
    original = static_cast<HBITMAP>(SelectObject(buffer, bitmap));
    size = {width, height};
    return buffer;
}

RECT App::WorkArea() const {
    POINT point{}; GetCursorPos(&point);
    const HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                    : MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{}; info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0); return work;
}

RECT App::ContentRect() const {
    RECT client{}; GetClientRect(window, &client);
    return {S(sidebarWidth), S(kHeaderHeight), client.right, std::max<LONG>(S(kHeaderHeight), client.bottom - S(kFooterHeight))};
}

void App::FitWindow(int x, int y, int width, int height) {
    const RECT work = WorkArea();
    const auto frame = ui::FitFrame({work.left, work.top, work.right, work.bottom}, {x, y, width, height}, dpi);
    SetWindowPos(window, nullptr, frame.x, frame.y, frame.width, frame.height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void App::InitialWindowSize() {
    if (const UINT actual = GetDpiForWindow(window)) systemDpi = static_cast<int>(actual);
    Scale(false);
    const RECT work = WorkArea();
    const ui::WorkArea area{work.left, work.top, work.right, work.bottom};
    auto frame = ui::InitialFrame(area, systemDpi, settings.uiScalePercent, kDefaultWindowWidth, kDefaultWindowHeight);
    SetWindowPos(window, nullptr, frame.x, frame.y, frame.width, frame.height, SWP_NOZORDER | SWP_NOACTIVATE);
    // Measure every page at this width (the window is not visible yet) so the
    // tallest page fits without scrolling wherever the desktop has the room.
    if (!controls.empty()) {
        const int current = page;
        int tallest = 0;
        for (unsigned n = 0; n < kPages.size(); ++n) { page = static_cast<int>(n); Layout(); tallest = std::max(tallest, contentHeight); }
        page = current;
        RECT client{}, bounds{};
        GetClientRect(window, &client); GetWindowRect(window, &bounds);
        const int chrome = (bounds.bottom - bounds.top) - client.bottom;
        const int wanted = S(kHeaderHeight + kFooterHeight + tallest + 10) + chrome;
        frame.y = work.top + (work.bottom - work.top - wanted) / 2;
        frame = ui::FitFrame(area, {frame.x, frame.y, frame.width, wanted}, dpi);
        SetWindowPos(window, nullptr, frame.x, frame.y, frame.width, frame.height, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
    }
}

void App::Scale(bool resize) {
    if (scaling) return;
    scaling = true;
    const int oldDpi = dpi;
    const RECT work = WorkArea();
    dpi = ui::EffectiveDpi(systemDpi, settings.uiScalePercent, {work.left, work.top, work.right, work.bottom});
    LoadIcons();
    if (window) {
        if (dpi != oldDpi) {
            scroll = MulDiv(scroll, dpi, oldDpi);
            Fonts();
        }
        if (resize) {
            RECT rect{}; GetWindowRect(window, &rect);
            FitWindow(rect.left, rect.top, MulDiv(rect.right - rect.left, dpi, oldDpi), MulDiv(rect.bottom - rect.top, dpi, oldDpi));
        }
        if (dpi != oldDpi) Layout();
    }
    scaling = false;
}

void App::EnsureVisible(HWND child) {
    if (syncing || !IsWindowVisible(child) || GetParent(child) != panel) return;
    RECT rect{}, client{}; GetWindowRect(child, &rect); GetClientRect(panel, &client);
    MapWindowPoints(nullptr, panel, reinterpret_cast<POINT*>(&rect), 2);
    if (rect.top < S(8)) { scroll += rect.top - S(16); Layout(); }
    else if (rect.bottom > client.bottom - S(8)) { scroll += rect.bottom - client.bottom + S(16); Layout(); }
}

bool App::ScrollKey(UINT key) {
    RECT client{}; GetClientRect(panel, &client);
    const int step = std::max(S(52), static_cast<int>(client.bottom) - S(48));
    switch (key) {
    case VK_PRIOR: scroll -= step; break;
    case VK_NEXT: scroll += step; break;
    case VK_HOME: scroll = 0; break;
    case VK_END: scroll = S(contentHeight); break;
    default: return false;
    }
    Layout(); return true;
}

void App::ScrollBy(int pixels) {
    scroll += pixels;
    Layout();
}

void App::DeleteFonts() {
    for (auto& font : fonts) { if (font) DeleteObject(font); font = nullptr; }
}

void App::Fonts() {
    DeleteFonts();
    const wchar_t* face = L"Arial";
    HDC dc = GetDC(window);
    for (const wchar_t* candidate : {L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma", L"Arial", L"DejaVu Sans", L"Liberation Sans"}) {
        LOGFONTW query{}; query.lfCharSet = DEFAULT_CHARSET;
        lstrcpynW(query.lfFaceName, candidate, LF_FACESIZE);
        bool found = false;
        EnumFontFamiliesExW(dc, &query, FoundFont, reinterpret_cast<LPARAM>(&found), 0);
        if (found) { face = candidate; break; }
    }
    ReleaseDC(window, dc);
    const bool segoe = std::wstring(face).rfind(L"Segoe UI", 0) == 0;
    const int headingWeight = segoe ? FW_SEMIBOLD : FW_BOLD;
    // Grayscale AA avoids distorted small bold glyphs in the DejaVu
    // fallback's ClearType rasterization. Normal Windows Segoe UI keeps CT.
    const DWORD quality = std::wstring(face) == L"DejaVu Sans" ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY;
    const auto make = [&](int tenthPoints, int weight) {
        return CreateFontW(-MulDiv(tenthPoints, dpi, 720), 0, 0, 0, weight, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            quality, DEFAULT_PITCH | FF_SWISS, face);
    };
    fonts[FontNormal] = make(95, FW_NORMAL);
    fonts[FontSmall] = make(85, FW_NORMAL);
    fonts[FontSemibold] = make(105, headingWeight);
    fonts[FontTitle] = make(150, headingWeight);
    for (const auto& entry : controls) {
        SendMessageW(entry.second, WM_SETFONT, reinterpret_cast<WPARAM>(fonts[FontNormal]), TRUE);
        wchar_t type[32]{}; GetClassNameW(entry.second, type, 32);
        if (_wcsicmp(type, L"COMBOBOX") == 0) {
            SendMessageW(entry.second, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), S(24));
            SendMessageW(entry.second, CB_SETITEMHEIGHT, 0, S(24));
        } else if (_wcsicmp(type, TRACKBAR_CLASSW) == 0) {
            // The native drag target must scale with the painted knob.
            SendMessageW(entry.second, TBM_SETTHUMBLENGTH, S(22), 0);
        }
    }
}

HWND App::Control(int id, const wchar_t* type, const wchar_t* text, DWORD style) {
    const bool textField = std::wstring(type) == L"EDIT";
    const bool onWindow = (id >= PageCursor && id <= PageShortcuts) || id == Enable || id == Hide || id == Refresh || id == Quit;
    if (textField) { style &= ~WS_BORDER; editFrames[id] = {}; }
    HWND child = CreateWindowExW(0, type, text,
        WS_CHILD | WS_TABSTOP | style, 0, 0, 0, 0,
        onWindow ? window : panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    controls[id] = child;
    SetWindowSubclass(child, FocusProc, 3, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(fonts[FontNormal]), TRUE);
    if (std::wstring(type) == L"BUTTON" && (style & BS_TYPEMASK) == BS_AUTOCHECKBOX) {
        SetWindowTheme(child, L"", L"");
        SetWindowSubclass(child, ToggleProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    if (textField || std::wstring(type) == L"COMBOBOX")
        SetWindowSubclass(child, WheelProc, 5, reinterpret_cast<DWORD_PTR>(this));
    return child;
}

void App::Checked(int id, bool value) { SendMessageW(controls[id], BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0); }
bool App::Checked(int id) const { return SendMessageW(controls.at(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

void App::CreateControls() {
    Fonts();
    const auto toggle = [&](int id, const wchar_t* label) { Control(id, L"BUTTON", label, BS_AUTOCHECKBOX); };
    const auto button = [&](int id, const wchar_t* label) { Control(id, L"BUTTON", label, BS_OWNERDRAW); };
    for (const auto& pageSpec : kPages) button(pageSpec.id, pageSpec.label);
    toggle(Enable, L"Enable ClickShade");
    button(Hide, L"Keep in tray"); button(Refresh, L"Reload cursors"); button(Quit, L"Quit");
    for (int n = 0; n < kCardResetCount; ++n) button(kCardResetBase + n, L"Reset");

    toggle(DarkCursor, L"Change cursor on press");
    toggle(CursorReverse, L"Reverse light and dark");
    toggle(CursorHold, L"Keep the pressed cursor while holding a button");
    toggle(CursorShadow, L"Soft shadow under the cursor");
    toggle(CursorColorsEnable, L"Paint the cursor in my colors");
    button(CursorFillColor, L"Fill color"); button(CursorOutlineColor, L"Outline color");
    for (const auto& item : std::array<std::pair<int, const wchar_t*>, 5>{{
        {Left, L"Left"}, {Right, L"Right"}, {Middle, L"Middle"}, {Side1, L"Side 1"}, {Side2, L"Side 2"}}}) toggle(item.first, item.second);
    button(CursorCurrent, L"Your Windows theme"); button(CursorWindows, L"Windows arrow");
    button(CursorClassic, L"Classic arrow"); button(CursorCustom, L"Custom pair");
    toggle(PauseFullscreen, L"Pause all effects in fullscreen applications");
    toggle(PauseConfined, L"Pause when an application confines the pointer");
    HWND exclusions = Control(Excluded, L"EDIT", settings.excludedProcesses.c_str(), ES_AUTOHSCROLL);
    SendMessageW(exclusions, EM_SETLIMITTEXT, 2000, 0);
    SendMessageW(exclusions, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"game.exe; another.exe"));
    button(ApplyExcluded, L"Apply"); button(ExcludeBrowse, L"Add app...");
    HWND allowed = Control(Allowed, L"EDIT", settings.allowedProcesses.c_str(), ES_AUTOHSCROLL);
    SendMessageW(allowed, EM_SETLIMITTEXT, 2000, 0);
    SendMessageW(allowed, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"editor.exe; slides.exe"));
    button(ApplyAllowed, L"Apply"); button(AllowBrowse, L"Add app..."); button(ClearAllowed, L"Clear");
    toggle(RunAsAdmin, L"Run ClickShade as administrator");
    toggle(CaptureVisible, L"Show effects in screenshots and recordings");
    button(ThemeMint, L"Midnight Mint"); button(ThemeBlue, L"Ocean"); button(ThemeRose, L"Rose"); button(ThemeLight, L"Daylight");
    button(ThemeBackground, L"Background color"); button(ThemeAccent, L"Accent color");
    button(ThemeDarkMode, L"Dark"); button(ThemeLightMode, L"Light");
    toggle(HighlightEnable, L"Show cursor highlight");
    button(HighlightColor, L"Highlight color");
    button(HighlightFilledStyle, L"Filled"); button(HighlightRingStyle, L"Ring"); button(HighlightGlowStyle, L"Glow");
    button(HighlightCrosshairStyle, L"Crosshair"); button(HighlightSpotlightStyle, L"Spotlight");
    toggle(HighlightBreathing, L"Gently pulse while the pointer rests");
    toggle(IdleHideEnable, L"Hide the highlight when the mouse is idle");
    toggle(TrailEnable, L"Show cursor trail"); button(TrailColor, L"Trail color");
    button(TrailDots, L"Dots"); button(TrailRings, L"Rings"); button(TrailSparkles, L"Sparkles");
    button(TrailComet, L"Comet"); button(TrailFineSparkle, L"Fine sparkle");
    toggle(ClicksEnable, L"Show click effects");
    toggle(ClickLeft, L"Left button"); toggle(ClickRight, L"Right button"); toggle(ClickMiddle, L"Middle button");
    button(ColorLeft, L"Left color"); button(ColorRight, L"Right color"); button(ColorMiddle, L"Middle color");
    button(ClickRingStyle, L"Ring"); button(ClickPulseStyle, L"Pulse"); button(ClickRippleStyle, L"Ripple");
    button(ClickBurstStyle, L"Burst"); button(ClickNeonStyle, L"Neon ring"); button(ClickTargetStyle, L"Target");
    button(EaseLinear, L"Linear"); button(EaseCubic, L"Soft ease-out"); button(EaseSmooth, L"Smooth step");
    toggle(RefreshAuto, L"Auto: adapt to this display and render workload");
    toggle(UiScaleAuto, L"Automatic display and resolution scaling");
    toggle(AnimatedAccent, L"Animated spectrum line");
    toggle(SoundEnable, L"Play sounds on mouse press");
    toggle(KeyboardEnable, L"Play sounds while typing");
    button(PresetSubtle, L"Subtle"); button(PresetPresenter, L"Presentation"); button(PresetBold, L"High visibility");
    button(RestoreDefaults, L"Restore all defaults...");
    button(ShakeOff, L"Off"); button(ShakePlasma, L"Plasma style"); button(ShakeMac, L"macOS inspired");
    toggle(MagnifierEnable, L"Show a magnifier lens at the pointer");
    button(MagnifierBorderColor, L"Frame color");
    button(MagnifierCircle, L"Round"); button(MagnifierSquare, L"Rounded square");
    button(MagnifierCentered, L"Centered on the pointer"); button(MagnifierBeside, L"Beside the pointer");
    toggle(MagnifierFrame, L"Draw a frame around the lens");
    toggle(KeyDisplayEnable, L"Show the keys I press on screen");
    button(KeyDisplayColor, L"Backdrop color");
    button(KeyDisplayShortcuts, L"Shortcuts only"); button(KeyDisplayAll, L"Every key");
    toggle(KeyDisplayMouse, L"Also show mouse buttons");
    button(KeyDisplayBottom, L"Bottom center"); button(KeyDisplayTop, L"Top center");
    button(KeyDisplayBottomLeft, L"Bottom left"); button(KeyDisplayBottomRight, L"Bottom right");
    button(KeyDisplayTest, L"Show a sample");
    button(RefreshSoundLibrary, L"Refresh lists");
    button(ExportProfile, L"Export profile..."); button(BackupProfile, L"Back up current"); button(ImportProfile, L"Import profile...");
    button(OpenHomepage, L"Project page"); button(OpenSettingsFolder, L"Settings folder"); button(OpenCrashFolder, L"Crash reports");
    for (unsigned n = 0; n < kHotkeys.size(); ++n) {
        HWND field = Control(HotkeyFieldBase + n, L"EDIT", L"", ES_AUTOHSCROLL);
        SendMessageW(field, EM_SETLIMITTEXT, 80, 0);
        SendMessageW(field, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Unassigned"));
        SetWindowSubclass(field, ShortcutProc, 4, reinterpret_cast<DWORD_PTR>(this));
        button(HotkeyRecordBase + n, L"Record"); button(HotkeyClearBase + n, L"Clear");
    }
    button(HotkeyApply, L"Apply shortcuts");
    for (const auto& spec : kSliders) {
        HWND slider = Control(spec.id, TRACKBAR_CLASSW, spec.label, TBS_HORZ | TBS_NOTICKS | TBS_FIXEDLENGTH);
        SendMessageW(slider, TBM_SETRANGEMIN, FALSE, spec.minimum);
        SendMessageW(slider, TBM_SETRANGEMAX, FALSE, spec.maximum);
        SendMessageW(slider, TBM_SETPAGESIZE, 0, std::max(1, (spec.maximum - spec.minimum) / 10));
        SendMessageW(slider, TBM_SETTHUMBLENGTH, S(22), 0);
        SetWindowTheme(slider, L"", L"");
        SetWindowSubclass(slider, SliderProc, 2, reinterpret_cast<DWORD_PTR>(this));
        HWND field = Control(spec.id + kValueOffset, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER);
        SendMessageW(field, EM_SETLIMITTEXT, 5, 0);
        button(spec.id + kResetOffset, L"↺");
    }
    for (const auto& sound : kSounds) {
        Control(sound.field, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY);
        HWND combo = Control(sound.preset, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL);
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), S(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, S(24));
        button(sound.browse, L"Browse..."); button(sound.test, L"Test"); button(sound.clear, L"Clear");
    }
    Control(KeyboardPath, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY);
    HWND keyboardCombo = Control(KeyboardSet, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL);
    SendMessageW(keyboardCombo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), S(24));
    SendMessageW(keyboardCombo, CB_SETITEMHEIGHT, 0, S(24));
    button(KeyboardBrowseFolder, L"Folder..."); button(KeyboardBrowseFile, L"File..."); button(KeyboardTest, L"Test"); button(KeyboardClear, L"Clear");
    for (int id : {ReleasedFile, PressedFile}) Control(id, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY);
    for (int id : {BrowseReleased, BrowsePressed}) button(id, L"Browse...");
    button(ClearPressed, L"Auto dark");
    toggle(Startup, L"Start with Windows"); toggle(Minimized, L"Open in the system tray");
    RefreshSounds(); SyncControls(); Layout();
}

const SliderSpec* App::Slider(int id) const {
    for (const auto& spec : kSliders) if (spec.id == id) return &spec;
    return nullptr;
}

bool App::SliderEnabled(int id) const {
    switch (id) {
    case CursorFlash: return !settings.cursorHold;
    case RefreshRate: return settings.visualRefreshRate != 0;
    case UiScale: return settings.uiScalePercent != 0;
    case HighlightThickness: return settings.highlightStyle == kHighlightRing || settings.highlightStyle == kHighlightCrosshair;
    case HighlightOpacity: case HighlightRadius: return settings.highlightStyle != kHighlightSpotlight;
    case IdleHideDelay: return settings.highlightIdleHideMs != 0;
    case ShakeSensitivity: case ShakeMaximum: return settings.shakeStyle != 0;
    case SoundVolume: return true;
    default: return true;
    }
}

void App::RefreshSounds() {
    soundChoices = FindSoundChoices();
    keyboardSets = FindKeyboardSets();
    const bool wasSyncing = syncing; syncing = true;
    for (const auto& sound : kSounds) {
        const HWND combo = controls[sound.preset];
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom file / no library selection"));
        for (const auto& choice : soundChoices)
            if (choice.buttons & sound.mask) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label.c_str()));
    }
    const HWND keyboard = controls[KeyboardSet];
    SendMessageW(keyboard, CB_RESETCONTENT, 0, 0);
    SendMessageW(keyboard, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom folder or file / no library selection"));
    for (const auto& set : keyboardSets) SendMessageW(keyboard, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(set.label.c_str()));
    syncing = wasSyncing;
    SyncControls();
}

void App::SyncControls() {
    syncing = true;
    for (const auto& spec : kToggles) Checked(spec.id, settings.*spec.member);
    Checked(RunAsAdmin, settings.runAsAdministrator);
    Checked(RefreshAuto, settings.visualRefreshRate == 0); Checked(UiScaleAuto, settings.uiScalePercent == 0);
    Checked(IdleHideEnable, settings.highlightIdleHideMs != 0);
    const std::array<int, 5> ids{Left, Right, Middle, Side1, Side2};
    for (unsigned n = 0; n < ids.size(); ++n) Checked(ids[n], (settings.buttonMask & (1u << n)) != 0);
    const Settings defaults;
    for (const auto& spec : kSliders) {
        const int value = settings.*(spec.member);
        const int displayed = value ? value : spec.id == RefreshRate ? 144 : spec.id == IdleHideDelay ? 2000 : spec.minimum;
        SendMessageW(controls[spec.id], TBM_SETPOS, TRUE, displayed);
        SetWindowTextW(controls[spec.id + kValueOffset], std::to_wstring(displayed).c_str());
        const bool enabled = SliderEnabled(spec.id);
        EnableWindow(controls[spec.id], enabled);
        EnableWindow(controls[spec.id + kValueOffset], enabled);
        EnableWindow(controls[spec.id + kResetOffset], enabled && value != defaults.*(spec.member));
    }
    for (int n = 0; n < kCardResetCount; ++n)
        EnableWindow(controls[kCardResetBase + n], n < static_cast<int>(cardChanged.size()) && cardChanged[n]());
    for (const auto& sound : kSounds) {
        const auto& path = settings.*sound.member;
        SetWindowTextW(controls[sound.field], path.empty() ? L"No sound selected" : path.c_str());
        EnableWindow(controls[sound.test], started && !path.empty());
        unsigned selected = 0, index = 0;
        for (const auto& choice : soundChoices) {
            if (!(choice.buttons & sound.mask)) continue;
            ++index;
            if (SameFilePath(path, choice.path)) { selected = index; break; }
        }
        SendMessageW(controls[sound.preset], CB_SETCURSEL, selected, 0);
    }
    SetWindowTextW(controls[KeyboardPath], settings.keyboardSoundPath.empty() ? L"None selected" : settings.keyboardSoundPath.c_str());
    EnableWindow(controls[KeyboardTest], started && !settings.keyboardSoundPath.empty());
    {
        unsigned selected = 0;
        for (unsigned n = 0; n < keyboardSets.size(); ++n)
            if (SameFilePath(settings.keyboardSoundPath, keyboardSets[n].path)) { selected = n + 1; break; }
        SendMessageW(controls[KeyboardSet], CB_SETCURSEL, selected, 0);
    }
    SetWindowTextW(controls[ReleasedFile], settings.releasedCursorPath.empty() ? L"Choose a static .cur file" : settings.releasedCursorPath.c_str());
    SetWindowTextW(controls[PressedFile], settings.pressedCursorPath.empty() ? L"Automatically invert the released cursor" : settings.pressedCursorPath.c_str());
    if (!hotkeyDraftDirty && capturingShortcut < 0)
        for (unsigned n = 0; n < kHotkeys.size(); ++n)
            SetWindowTextW(controls[HotkeyFieldBase + n], (settings.*kHotkeys[n].member).c_str());
    syncing = false;
}

void App::Layout() {
    if (!window || !panel || controls.empty()) return;
    RECT client{}; GetClientRect(window, &client);
    const int widthDip = D(client.right), heightDip = D(client.bottom);
    sidebarWidth = widthDip < 860 ? kSidebarCompactWidth : kSidebarWidth;
    const RECT content = ContentRect();
    const int contentWidth = D(content.right - content.left);
    const int contentHeightPx = content.bottom - content.top;
    // Controls and painted text must share one scroll offset. Clamp against
    // the last known page height before placing anything; the final clamp
    // below repeats the layout once if the measured height moved the limit.
    scroll = std::clamp(scroll, 0, std::max(0, S(contentHeight) - contentHeightPx));
    columns = contentWidth - 2 * kContentMargin >= kTwoColumnMinimum ? 2 : 1;
    std::map<int, bool> visible;
    paintTexts.clear(); paintFrames.clear(); cardResets.clear(); cardChanged.clear();

    // Fixed chrome: sidebar navigation, master switch, footer buttons.
    const auto putWindow = [&](int id, int x, int y, int w, int h) {
        visible[id] = true;
        MoveWindow(controls[id], S(x), S(y), S(std::max(1, w)), S(h), FALSE);
    };
    const bool compact = sidebarWidth == kSidebarCompactWidth;
    for (unsigned n = 0; n < kPages.size(); ++n)
        putWindow(kPages[n].id, compact ? 8 : 12, kSidebarNavTop + static_cast<int>(n) * kSidebarNavStep, sidebarWidth - (compact ? 16 : 24), 36);
    const int statusTop = heightDip - kFooterHeight - (compact ? 40 : StatusBlockHeight(heightDip));
    if (!compact) putWindow(Enable, 16, statusTop, sidebarWidth - 32, 26);
    else putWindow(Enable, 8, statusTop, 44, 26);
    const int footerY = heightDip - kFooterHeight + 9;
    putWindow(Quit, widthDip - 96, footerY, 80, 32);
    putWindow(Hide, widthDip - 96 - 8 - 112, footerY, 112, 32);
    putWindow(Refresh, widthDip - 96 - 8 - 112 - 8 - 124, footerY, 124, 32);

    // Scrolling content.
    const int columnWidth = columns == 2 ? (contentWidth - 2 * kContentMargin - kCardGap) / 2 : contentWidth - 2 * kContentMargin;
    int columnY[2] = {kContentMargin - 4, kContentMargin - 4};
    const int columnX[2] = {kContentMargin, kContentMargin + columnWidth + kCardGap};
    int resetIndex = 0;
    const Settings defaults;
    // column: 0, 1 or 2 for full width.
    const auto card = [&](int column, const wchar_t* title, std::function<void(CardBuilder&)> build,
                          std::function<void()> reset = {}, std::function<bool()> changed = {}) {
        const int useColumn = columns == 1 ? 0 : column;
        const int x = columnX[useColumn == 2 ? 0 : useColumn];
        const int width = useColumn == 2 && columns == 2 ? columnWidth * 2 + kCardGap : columnWidth;
        const int y = useColumn == 2 ? std::max(columnY[0], columnY[1]) : columnY[useColumn];
        const int index = reset ? resetIndex++ : -1;
        if (reset) { cardResets.push_back(std::move(reset)); cardChanged.push_back(std::move(changed)); }
        CardBuilder builder(*this, x, y, width, title, visible, index);
        build(builder);
        const int bottom = builder.Finish() + kCardGap;
        if (useColumn == 2) columnY[0] = columnY[1] = bottom; else columnY[useColumn] = bottom;
    };
#define FIELD(member) [this, &defaults] { settings.member = defaults.member; }, [this, &defaults] { return settings.member != defaults.member; }
    struct FieldPair { std::function<void()> reset; std::function<bool()> changed; };
    struct Group { std::function<void()> first; std::function<bool()> second; };
    const auto group = [&](std::vector<FieldPair> pairs) {
        std::vector<std::function<void()>> resets; std::vector<std::function<bool()>> checks;
        for (auto& pair : pairs) { resets.push_back(pair.reset); checks.push_back(pair.changed); }
        Group result;
        result.first = [resets] { for (auto& action : resets) action(); };
        result.second = [checks] { for (auto& check : checks) if (check()) return true; return false; };
        return result;
    };

    if (page == 0) {
        auto g = group({{FIELD(cursorStyle)}, {FIELD(releasedCursorPath)}, {FIELD(pressedCursorPath)}});
        card(2, L"Choose your cursor", [&](CardBuilder& c) {
            c.Buttons({CursorCurrent, CursorWindows, CursorClassic, CursorCustom}, columns == 2 ? 4 : 2, 36);
            const wchar_t* descriptions[]{L"Keep your installed static cursor theme, including custom Windows cursors.",
                L"Use Windows' own default arrow from this computer. Other cursor shapes keep your theme.",
                L"A crisp classic-style arrow, drawn for ClickShade. Other shapes keep your theme.",
                L"Choose released and pressed .cur artwork below. Other shapes keep your theme."};
            c.Hint(descriptions[std::clamp(settings.cursorStyle, 0, 3)]);
            if (settings.cursorStyle == 3) {
                c.Text(L"Released cursor", FontSmall, 20); c.Field(ReleasedFile, {{BrowseReleased, 96}});
                c.Text(L"Pressed cursor", FontSmall, 20); c.Field(PressedFile, {{BrowsePressed, 96}, {ClearPressed, 92}});
                c.Hint(L"Matching cursor filenames pair automatically. Auto dark uses inversion; .ani files stay untouched.");
            }
        }, g.first, g.second);
        g = group({{FIELD(darkCursorEnabled)}, {FIELD(cursorReverse)}, {FIELD(cursorHold)}, {FIELD(buttonMask)}});
        card(0, L"Press behavior", [&](CardBuilder& c) {
            c.Toggle(DarkCursor); c.Toggle(CursorReverse); c.Toggle(CursorHold);
            c.Text(L"Buttons that change the cursor", FontSmall, 22);
            c.CompactToggles({Left, Right, Middle, Side1, Side2});
        }, g.first, g.second);
        g = group({{FIELD(cursorScale)}, {FIELD(cursorFlashMs)}});
        card(1, L"Size and timing", [&](CardBuilder& c) {
            c.Slider(CursorSize); c.Slider(CursorFlash);
            c.Hint(L"100% keeps the size Windows uses. Larger scales redraw every pointer shape, animated ones included, from its sharpest source image, up to 256 pixels. Brief press applies when the pressed cursor is not held.");
        }, g.first, g.second);
        g = group({{FIELD(cursorOpacity)}, {FIELD(cursorShadow)}, {FIELD(cursorColors)}, {FIELD(cursorFillColor)}, {FIELD(cursorOutlineColor)}});
        card(2, L"Cursor look", [&](CardBuilder& c) {
            c.Slider(CursorOpacity);
            c.Toggle(CursorShadow); c.Toggle(CursorColorsEnable);
            c.Buttons({CursorFillColor, CursorOutlineColor}, 2, 32);
            c.Hint(L"Opacity, shadow and colors apply to every pointer shape, animated ones included. Colors repaint the light parts with the fill and the dark parts with the outline, so the pressed cursor shows them swapped.");
        }, g.first, g.second);
        g = group({{FIELD(shakeStyle)}, {FIELD(shakeSensitivity)}, {FIELD(shakeMaximumScale)}});
        card(2, L"Shake to find your cursor", [&](CardBuilder& c) {
            c.Buttons({ShakeOff, ShakePlasma, ShakeMac}, 3, 34);
            c.Slider(ShakeSensitivity); c.Slider(ShakeMaximum);
            c.Hint(L"Plasma style follows KDE's published detector exactly (1 s window, 100 px span, path 4x the span, 3x then +1x, 200 ms, 2 s hold). macOS inspired is ClickShade's own quicker tuning. Off by default; pause rules still apply.");
        }, g.first, g.second);
    } else if (page == 1) {
        auto g = group({{FIELD(highlightEnabled)}, {FIELD(highlightStyle)}, {FIELD(highlightColor)}, {FIELD(highlightRadius)}, {FIELD(highlightOpacity)},
                        {FIELD(highlightThickness)}, {FIELD(spotlightRadius)}, {FIELD(spotlightDim)}, {FIELD(highlightBreathing)}, {FIELD(highlightIdleHideMs)}});
        card(0, L"Pointer highlight", [&](CardBuilder& c) {
            c.ToggleWith(HighlightEnable, HighlightColor);
            c.Buttons({HighlightFilledStyle, HighlightRingStyle, HighlightGlowStyle, HighlightCrosshairStyle, HighlightSpotlightStyle}, 3, 32);
            if (settings.highlightStyle == kHighlightSpotlight) {
                c.Slider(SpotlightRadius); c.Slider(SpotlightDim);
                c.Hint(L"Spotlight dims the pointer's monitor around a clear circle. It stays click-through and follows the pointer at the display rate.");
            } else {
                c.Slider(HighlightRadius); c.Slider(HighlightOpacity); c.Slider(HighlightThickness);
            }
            c.Toggle(HighlightBreathing); c.Toggle(IdleHideEnable); c.Slider(IdleHideDelay);
            c.Hint(L"Sizes follow each monitor's Windows scaling. Ring and crosshair keep text under the pointer clear.");
        }, g.first, g.second);
        g = group({{FIELD(trailEnabled)}, {FIELD(trailStyle)}, {FIELD(trailColor)}, {FIELD(trailRadius)}, {FIELD(trailOpacity)}, {FIELD(trailDurationMs)}});
        card(1, L"Cursor trails", [&](CardBuilder& c) {
            c.ToggleWith(TrailEnable, TrailColor);
            c.Buttons({TrailDots, TrailRings, TrailSparkles, TrailComet, TrailFineSparkle}, 3, 32);
            c.Slider(TrailRadius); c.Slider(TrailOpacity); c.Slider(TrailDuration);
            c.Hint(L"A short fading trail follows your pointer without changing its movement. At most 32 stamps; off by default.");
        }, g.first, g.second);
        card(1, L"One-click effect presets", [&](CardBuilder& c) {
            c.Buttons({PresetSubtle, PresetPresenter, PresetBold}, 3, 34);
            c.Hint(L"Apply a balanced highlight and click animation. Your chosen colors stay yours.");
        });
    } else if (page == 2) {
        auto g = group({{FIELD(clickEffectsEnabled)}, {FIELD(leftClickEnabled)}, {FIELD(rightClickEnabled)}, {FIELD(middleClickEnabled)},
                        {FIELD(leftClickColor)}, {FIELD(rightClickColor)}, {FIELD(middleClickColor)}, {FIELD(clickStyle)}});
        card(0, L"Click effects", [&](CardBuilder& c) {
            c.Toggle(ClicksEnable);
            c.ToggleWith(ClickLeft, ColorLeft); c.ToggleWith(ClickRight, ColorRight); c.ToggleWith(ClickMiddle, ColorMiddle);
            c.Text(L"Shape", FontSmall, 20);
            c.Buttons({ClickRingStyle, ClickPulseStyle, ClickRippleStyle, ClickBurstStyle, ClickNeonStyle, ClickTargetStyle}, 3, 32);
            c.Hint(L"Effects start where you pressed; moving the pointer does not drag them. Target closes in on the click instead of expanding. Neon blue, neon red and neon green are the defaults.");
        }, g.first, g.second);
        g = group({{FIELD(clickRadius)}, {FIELD(clickDurationMs)}, {FIELD(clickOpacity)}, {FIELD(clickThickness)}, {FIELD(clickEasing)}});
        card(1, L"Size and timing", [&](CardBuilder& c) {
            c.Slider(ClickRadius); c.Slider(ClickDuration); c.Slider(ClickOpacity); c.Slider(ClickThickness);
            c.Text(L"Animation feel", FontSmall, 20);
            c.Buttons({EaseLinear, EaseCubic, EaseSmooth}, 3, 32);
            c.Hint(L"Radius and width follow your display scaling. Animation feel shapes how an effect grows and fades.");
        }, g.first, g.second);
    } else if (page == 3) {
        auto g = group({{FIELD(clickSoundsEnabled)}, {FIELD(soundVolume)}, {FIELD(leftSoundPath)}, {FIELD(rightSoundPath)}, {FIELD(middleSoundPath)}});
        card(0, L"Mouse clicks", [&](CardBuilder& c) {
            c.Toggle(SoundEnable); c.Slider(SoundVolume);
            for (const auto& sound : kSounds) {
                c.Text(sound.label, FontSmall, 20);
                c.Combo(sound.preset);
                c.Field(sound.field, {{sound.browse, 82}, {sound.test, 56}, {sound.clear, 60}});
            }
            c.Hint(L"Each list shows only the files made for that button, plus any file without a button name. Browse starts in the sounds folder; WAV, MP3, WMA, FLAC, M4A and AAC work.");
            c.Buttons({RefreshSoundLibrary}, columns == 2 ? 2 : 3, 30);
        }, g.first, g.second);
        g = group({{FIELD(keyboardSoundsEnabled)}, {FIELD(keyboardVolume)}, {FIELD(keyboardSoundPath)}});
        card(1, L"Keyboard typing", [&](CardBuilder& c) {
            c.Toggle(KeyboardEnable); c.Slider(KeyboardVolume);
            c.Text(L"Sound set", FontSmall, 20);
            c.Combo(KeyboardSet);
            c.Field(KeyboardPath, {{KeyboardBrowseFolder, 78}, {KeyboardBrowseFile, 60}, {KeyboardTest, 56}, {KeyboardClear, 60}});
            c.Hint(L"A set is a folder with key-1.wav ... key-6.wav plus optional space, enter and backspace files; a single file works for every key. Held keys do not repeat the sound.");
            c.Hint(L"Privacy: keyboard input is observed only while this switch is on, and ClickShade only distinguishes letter keys, Space, Enter and Backspace. It never records, stores or sends which keys you press.");
        }, g.first, g.second);
    } else if (page == kPresenterPage) {
        auto g = group({{FIELD(magnifierEnabled)}, {FIELD(magnifierZoom)}, {FIELD(magnifierSize)}, {FIELD(magnifierShape)},
                        {FIELD(magnifierPosition)}, {FIELD(magnifierBorderColor)}, {FIELD(magnifierFrame)}});
        card(0, L"Screen magnifier", [&](CardBuilder& c) {
            c.ToggleWith(MagnifierEnable, MagnifierBorderColor);
            c.Text(L"Shape", FontSmall, 20);
            c.Buttons({MagnifierCircle, MagnifierSquare}, 2, 32);
            c.Text(L"Placement", FontSmall, 20);
            c.Buttons({MagnifierCentered, MagnifierBeside}, 2, 32);
            c.Toggle(MagnifierFrame);
            c.Slider(MagnifierZoom); c.Slider(MagnifierSize);
            c.Hint(L"A live, click-through lens built on the Windows Magnification API. Centered puts the pointer in the middle of the lens; Beside keeps the lens out of the way and shows the pointer inside it. ClickShade's own effects stay out of the magnified view. The pause rules apply, but the lens stays put while you type.");
        }, g.first, g.second);
        g = group({{FIELD(keyDisplayEnabled)}, {FIELD(keyDisplayMode)}, {FIELD(keyDisplayMouse)}, {FIELD(keyDisplayPosition)},
                   {FIELD(keyDisplaySize)}, {FIELD(keyDisplayDurationMs)}, {FIELD(keyDisplayOpacity)}, {FIELD(keyDisplayColor)}});
        card(1, L"Key display", [&](CardBuilder& c) {
            c.ToggleWith(KeyDisplayEnable, KeyDisplayColor);
            c.Text(L"What to show", FontSmall, 20);
            c.Buttons({KeyDisplayShortcuts, KeyDisplayAll}, 2, 32);
            c.Toggle(KeyDisplayMouse);
            c.Text(L"Position", FontSmall, 20);
            c.Buttons({KeyDisplayBottom, KeyDisplayTop, KeyDisplayBottomLeft, KeyDisplayBottomRight}, 2, 32);
            c.Slider(KeyDisplaySize); c.Slider(KeyDisplayDuration); c.Slider(KeyDisplayOpacity);
            c.Buttons({KeyDisplayTest}, columns == 2 ? 2 : 3, 30);
            c.Hint(L"Shortcuts appear as keycaps on the active monitor and fade after the hold time; with Every key, typed letters join into words. Shortcuts only shows Ctrl, Alt or Win chords and function keys, so typed text never appears.");
            c.Hint(L"Privacy: keyboard input is observed only while this switch or keyboard sounds is on, and a key exists only while its row is on screen. Nothing is recorded. Excluded apps, fullscreen rules and the lock screen still pause it; a hidden pointer does not.");
        }, g.first, g.second);
    } else if (page == kPresenterPage + 1) {
        auto g = group({{FIELD(pauseFullscreen)}, {FIELD(pauseConfinedCursor)}, {FIELD(excludedProcesses)}, {FIELD(allowedProcesses)}});
        card(0, L"Smart pause", [&](CardBuilder& c) {
            c.Toggle(PauseFullscreen); c.Toggle(PauseConfined);
            c.Text(L"Excluded applications", FontSmall, 20);
            c.Field(Excluded, {{ExcludeBrowse, 90}, {ApplyExcluded, 62}});
            c.Hint(L"Separate executable names with semicolons, or choose an application with Add app. For windowed games, add their .exe here.");
            c.Text(L"Always allow in these apps", FontSmall, 20);
            c.Field(Allowed, {{AllowBrowse, 90}, {ClearAllowed, 58}, {ApplyAllowed, 62}});
            c.Hint(L"Allowed apps bypass the fullscreen and confined-pointer rules. Exclusions, screen locks and hidden pointers still pause effects.");
        }, g.first, g.second);
        card(1, L"Administrator windows", [&](CardBuilder& c) {
            c.Toggle(RunAsAdmin);
            c.Hint(elevated ? L"ClickShade is running as administrator, so effects, shortcuts and held-button tracking also work over Task Manager, installers and other elevated windows."
                            : L"Windows keeps normal apps from fully reacting to windows that run as administrator (Task Manager, installers, some games). Turn this on to run ClickShade elevated: one prompt per manual start, none when it starts with Windows.",
                   elevated ? colors.success : CLR_INVALID);
            if (!compatibilityNote.empty()) c.Hint(compatibilityNote, colors.warning);
        });
        g = group({{FIELD(captureVisible)}});
        card(1, L"Screen recording", [&](CardBuilder& c) {
            c.Toggle(CaptureVisible);
            c.Hint(L"On: OBS, ShareX, Snipping Tool, Discord and other capture tools record the effects together with your screen. Off: the effects stay on your display only (Windows 10 version 2004 or later).");
        }, g.first, g.second);
        g = group({{FIELD(startMinimized)}});
        card(1, L"Startup and background", [&](CardBuilder& c) {
            c.Toggle(Startup); c.Toggle(Minimized);
            c.Hint(settings.runAsAdministrator ? L"With administrator mode on, startup uses a scheduled task so no prompt appears at sign-in. Closing the settings window keeps ClickShade in the tray; Quit stops it and restores your cursors."
                                               : L"Closing the settings window keeps ClickShade in the tray. Quit stops it and restores your cursors.");
        }, g.first, g.second);
    } else if (page == kAppearancePage) {
        auto g = group({{FIELD(uiDarkMode)}, {FIELD(uiBackground)}, {FIELD(uiAccent)}, {FIELD(uiDarkBackground)}, {FIELD(uiDarkAccent)}, {FIELD(uiLightBackground)}, {FIELD(uiLightAccent)}});
        card(0, L"Theme", [&](CardBuilder& c) {
            c.Buttons({ThemeDarkMode, ThemeLightMode}, 2, 32);
            c.Buttons({ThemeMint, ThemeBlue, ThemeRose, ThemeLight}, 2, 40);
            c.Buttons({ThemeBackground, ThemeAccent}, 2, 36);
            wchar_t detail[120]{};
            swprintf(detail, 120, L"Background RGB %u, %u, %u   Accent RGB %u, %u, %u", GetRValue(settings.uiBackground), GetGValue(settings.uiBackground),
                     GetBValue(settings.uiBackground), GetRValue(settings.uiAccent), GetGValue(settings.uiAccent), GetBValue(settings.uiAccent));
            c.Hint(detail);
            c.Hint(L"Dark and Light each remember their own colors. Surfaces and text adjust for readability; effect colors are separate.");
        }, g.first, g.second);
        g = group({{FIELD(uiScalePercent)}, {FIELD(uiAnimatedAccent)}});
        card(1, L"Display and motion", [&](CardBuilder& c) {
            c.Toggle(UiScaleAuto); c.Slider(UiScale); c.Toggle(AnimatedAccent);
            c.Hint(L"Windows DPI is always respected. Auto adds roomier scaling on large desktops. The spectrum line only animates while this window is open.");
        }, g.first, g.second);
        g = group({{FIELD(visualRefreshRate)}});
        card(1, L"Frame pacing", [&](CardBuilder& c) {
            c.Toggle(RefreshAuto); c.Slider(RefreshRate);
            c.Hint(L"Auto paces click effects and trails by ClickShade's own drawing work, up to this display's refresh rate, while the highlight, spotlight and magnifier follow the pointer at the full display rate. A manual limit caps all of them. Breathing, key display fades and the cursor itself keep their own timing.");
        }, g.first, g.second);
        card(1, L"Settings profiles and backups", [&](CardBuilder& c) {
            c.Buttons({ExportProfile, BackupProfile, ImportProfile}, columns == 2 ? 1 : 3, 32);
            c.Hint(L"Export every preference, keep one manual backup, or import a Windows profile. Import keeps this PC's startup and administrator choices and saves the previous settings first.");
        });
        card(0, L"Restore defaults", [&](CardBuilder& c) {
            c.Buttons({RestoreDefaults}, columns == 2 ? 1 : 2, 34);
            c.Hint(L"Every card also has its own Reset, and each slider a small reset arrow. This button resets everything, including startup, administrator mode and tray preferences. Your sound and cursor files stay on disk.");
        });
        card(2, L"About ClickShade", [&](CardBuilder& c) {
            c.Hint(std::wstring(L"Version ") + kClickShadeVersionW + (elevated ? L" (administrator)" : L"") + (PortableMode() ? L", portable mode" : L"") +
                   L". Open source under the MIT license. No telemetry, no account, no network service. Crash reports are only written when something goes wrong and never leave this computer.");
            c.Buttons({OpenHomepage, OpenSettingsFolder, OpenCrashFolder}, 3, 32);
            if (crashNotice.present)
                c.Hint(L"Recovered after an internal error: " + crashNotice.summary + (crashNotice.reportPath.empty() ? L"" : L" Report: " + crashNotice.reportPath), colors.warning);
        });
    } else {
        auto g = group({{FIELD(hotkeyToggleAll)}, {FIELD(hotkeyHighlight)}, {FIELD(hotkeyClicks)}, {FIELD(hotkeySounds)}, {FIELD(hotkeyKeyboardSounds)},
                        {FIELD(hotkeyMagnifier)}, {FIELD(hotkeyKeyDisplay)}, {FIELD(hotkeySettings)}, {FIELD(hotkeyRestore)}});
        card(2, L"Global shortcuts", [&](CardBuilder& c) {
            for (unsigned n = 0; n < kHotkeys.size(); ++n) {
                c.Text(kHotkeys[n].label, FontSmall, 20);
                c.Field(HotkeyFieldBase + n, {{HotkeyRecordBase + n, 82}, {HotkeyClearBase + n, 64}});
            }
            c.Buttons({HotkeyApply}, columns == 2 ? 3 : 2, 34);
            c.Hint(shortcutStatus.empty() ? L"Use Ctrl, Alt or Win with a key. Record a combination, type it, or Clear it; then choose Apply shortcuts." : shortcutStatus,
                   shortcutError ? colors.warning : CLR_INVALID);
        }, g.first, g.second);
    }
#undef FIELD

    contentHeight = std::max(columnY[0], columnY[1]) + 6;
    const int clampedScroll = std::clamp(scroll, 0, std::max(0, S(contentHeight) - contentHeightPx));
    if (clampedScroll != scroll) { scroll = clampedScroll; Layout(); return; }
    MoveWindow(panel, content.left, content.top, content.right - content.left, contentHeightPx, FALSE);
    // Reset buttons show only when their card differs from the defaults.
    for (int n = 0; n < kCardResetCount; ++n) {
        const bool present = n < static_cast<int>(cardChanged.size());
        if (!present) visible[kCardResetBase + n] = false;
        else EnableWindow(controls[kCardResetBase + n], cardChanged[n]());
    }
    for (const auto& entry : controls) {
        const bool show = visible[entry.first];
        if (show != ((GetWindowLongPtrW(entry.second, GWL_STYLE) & WS_VISIBLE) != 0)) ShowWindow(entry.second, show ? SW_SHOWNA : SW_HIDE);
        if (show) InvalidateRect(entry.second, nullptr, FALSE);
    }
    InvalidateRect(window, nullptr, FALSE);
    InvalidateRect(panel, nullptr, FALSE);
}

} // namespace clickshade::ui
