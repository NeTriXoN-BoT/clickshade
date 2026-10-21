#include "ui_common.h"
#include <dwmapi.h>
#include <cstdint>

namespace clickshade::ui {
namespace {
constexpr int kSpectrumGlow = 5; // Rows of glow above and below the 2 px line.

void TextOut(HDC dc, const std::wstring& value, RECT rect, HFONT font, COLORREF color, UINT flags) {
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, flags);
    SelectObject(dc, old);
}

void RoundBox(HDC dc, RECT rect, COLORREF fill, COLORREF stroke, int radius) {
    HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, stroke);
    HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}
} // namespace

void App::ApplyTheme() {
    colors = Palette(settings);
    for (HBRUSH brush : {background, sidebarBrush, cardBrush, editBrush}) if (brush) DeleteObject(brush);
    background = CreateSolidBrush(colors.background);
    sidebarBrush = CreateSolidBrush(colors.sidebar);
    cardBrush = CreateSolidBrush(colors.card);
    editBrush = CreateSolidBrush(colors.edit);
    if (window) {
        const BOOL dark = colors.dark ? TRUE : FALSE;
        DwmSetWindowAttribute(window, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        // Windows 11 lets the frame match the palette; Windows 10 ignores these.
        const COLORREF caption = colors.sidebar, border = colors.border, text = colors.text;
        DwmSetWindowAttribute(window, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
        DwmSetWindowAttribute(window, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
        DwmSetWindowAttribute(window, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
        const int rounded = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(window, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &rounded, sizeof(rounded));
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}

std::wstring App::StatusHeading() const {
    const bool coreError = !startupError.empty() || !status.lastError.empty();
    if (coreError) return !startupError.empty() ? L"Engine could not start" : L"A setting needs attention";
    if (!status.visualError.empty() || !status.audioError.empty()) return L"Running with a warning";
    if (status.pausedByRule) return L"Paused automatically";
    if (status.pressed) return L"Pressed appearance";
    return settings.enabled ? L"Ready for your next click" : L"All effects paused";
}

std::wstring App::StatusDetail() const {
    if (!startupError.empty()) return startupError;
    if (!status.lastError.empty()) return status.lastError;
    if (!status.visualError.empty()) return status.visualError;
    if (!status.audioError.empty()) return status.audioError;
    if (status.foregroundElevated && !elevated && settings.enabled)
        return L"An administrator window is in front. Turn on Run as administrator under Rules so effects work there too.";
    return status.stateText;
}

bool App::StatusIsWarning() const {
    return !startupError.empty() || !status.lastError.empty() || !status.visualError.empty() || !status.audioError.empty() ||
           (status.foregroundElevated && !elevated && settings.enabled);
}

void App::DrawSpectrum(HDC dc, const RECT& line) {
    const int width = line.right - line.left;
    const int rows = 2 * kSpectrumGlow + 2;
    if (width <= 0) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -rows;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); return; }
    auto* pixels = static_cast<std::uint32_t*>(bits);
    const auto pack = [](COLORREF c) { return (static_cast<std::uint32_t>(GetRValue(c)) << 16) | (GetGValue(c) << 8) | GetBValue(c); };
    const bool animated = settings.uiAnimatedAccent;
    const double baseHue = animated ? spectrumPhase : 0.0;
    for (int x = 0; x < width; ++x) {
        COLORREF color;
        if (animated) color = FromHsv(baseHue + 360.0 * x / std::max(1, width), colors.dark ? 0.78 : 0.9, colors.dark ? 1.0 : 0.82);
        else color = Mix(colors.accent, colors.background, static_cast<double>(x) / std::max(1, width) * 0.85);
        for (int row = 0; row < rows; ++row) {
            const int distance = row < kSpectrumGlow ? kSpectrumGlow - row : row >= kSpectrumGlow + 2 ? row - kSpectrumGlow - 1 : 0;
            double amount = 1.0;
            if (distance) amount = std::pow(0.55, distance) * (colors.dark ? 0.55 : 0.4);
            pixels[row * width + x] = pack(distance ? Mix(colors.background, color, amount) : color);
        }
    }
    HDC memory = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(dc, line.left, line.top - kSpectrumGlow, width, rows, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteDC(memory);
    DeleteObject(bitmap);
}

void App::StartSpectrum() {
    if (!window || spectrumTimerArmed || !settings.uiAnimatedAccent || !IsWindowVisible(window)) return;
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    if (!animations) return; // Respect the Windows "animation effects" accessibility setting.
    spectrumLast = GetTickCount64();
    spectrumTimerArmed = SetTimer(window, kSpectrumTimer, 33, nullptr) != 0;
}

void App::StopSpectrum() {
    if (spectrumTimerArmed) KillTimer(window, kSpectrumTimer);
    spectrumTimerArmed = false;
}

void App::TickSpectrum() {
    if (!settings.uiAnimatedAccent || !IsWindowVisible(window) || IsIconic(window)) { StopSpectrum(); return; }
    const ULONGLONG now = GetTickCount64();
    spectrumPhase = std::fmod(spectrumPhase + (now - spectrumLast) * 0.024, 360.0); // One full cycle every 15 s.
    spectrumLast = now;
    RECT client{}; GetClientRect(window, &client);
    const RECT line{0, client.bottom - S(kFooterHeight) - kSpectrumGlow - 1, client.right, client.bottom - S(kFooterHeight) + kSpectrumGlow + 2};
    InvalidateRect(window, &line, FALSE);
}

void App::DrawNavGlyph(HDC dc, int pageIndex, int x, int y, int size, COLORREF ink) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), ink);
    HBRUSH brush = CreateSolidBrush(ink);
    HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int cx = x + size / 2, cy = y + size / 2, r = size / 2 - S(1);
    switch (pageIndex) {
    case 0: { // Cursor arrow.
        POINT points[]{{x + S(3), y + S(2)}, {x + S(3), y + size - S(3)}, {x + S(7), y + size - S(7)}, {x + S(10), y + size - S(1)},
                       {x + S(13), y + size - S(3)}, {x + S(10), y + size - S(8)}, {x + size - S(2), y + size - S(8)}};
        SelectObject(dc, brush); Polygon(dc, points, 7); break;
    }
    case 1: Ellipse(dc, cx - r, cy - r, cx + r, cy + r); SelectObject(dc, brush); Ellipse(dc, cx - S(3), cy - S(3), cx + S(3), cy + S(3)); break;
    case 2: Ellipse(dc, cx - r, cy - r, cx + r, cy + r); Ellipse(dc, cx - r / 2, cy - r / 2, cx + r / 2, cy + r / 2); break;
    case 3: { // Speaker.
        POINT points[]{{x + S(2), cy - S(4)}, {x + S(6), cy - S(4)}, {x + S(11), cy - S(8)}, {x + S(11), cy + S(8)}, {x + S(6), cy + S(4)}, {x + S(2), cy + S(4)}};
        SelectObject(dc, brush); Polygon(dc, points, 6);
        Arc(dc, x + S(6), cy - S(7), x + S(18), cy + S(7), x + S(13), cy - S(6), x + S(13), cy + S(6)); break;
    }
    case 4: { // Magnifying glass.
        const int lens = r - S(2);
        Ellipse(dc, x + S(1), y + S(1), x + S(1) + lens * 2, y + S(1) + lens * 2);
        HPEN handle = CreatePen(PS_SOLID, std::max(1, S(3)), ink);
        HGDIOBJ previous = SelectObject(dc, handle);
        MoveToEx(dc, x + S(1) + lens * 2 - S(2), y + S(1) + lens * 2 - S(2), nullptr);
        LineTo(dc, x + size - S(1), y + size - S(1));
        SelectObject(dc, previous); DeleteObject(handle);
        break;
    }
    case 5: { // Shield.
        POINT points[]{{cx, y + S(2)}, {x + size - S(3), y + S(5)}, {x + size - S(4), cy + S(2)}, {cx, y + size - S(2)}, {x + S(4), cy + S(2)}, {x + S(3), y + S(5)}};
        Polygon(dc, points, 6); break;
    }
    case 6: { // Palette.
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, brush);
        for (const auto& dot : std::array<POINT, 3>{{{cx - S(4), cy - S(3)}, {cx + S(3), cy - S(4)}, {cx + S(4), cy + S(3)}}})
            Ellipse(dc, dot.x - S(2), dot.y - S(2), dot.x + S(2), dot.y + S(2));
        break;
    }
    default: { // Keycap.
        RoundRect(dc, x + S(2), y + S(4), x + size - S(2), y + size - S(4), S(4), S(4));
        MoveToEx(dc, x + S(6), cy + S(2), nullptr); LineTo(dc, x + size - S(6), cy + S(2)); break;
    }
    }
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
    DeleteObject(pen); DeleteObject(brush);
}

void App::Paint(HDC dc) {
    RECT client{}; GetClientRect(window, &client);
    FillRect(dc, &client, background);
    const bool compact = sidebarWidth == kSidebarCompactWidth;
    const int widthDip = D(client.right), heightDip = D(client.bottom);
    // Sidebar.
    const RECT sidebar{0, 0, S(sidebarWidth), client.bottom - S(kFooterHeight)};
    FillRect(dc, &sidebar, sidebarBrush);
    DrawIconEx(dc, S(compact ? 16 : 16), S(18), icon, S(28), S(28), 0, nullptr, DI_NORMAL);
    if (!compact) {
        TextOut(dc, L"ClickShade", {S(52), S(16), S(sidebarWidth), S(40)}, fonts[FontSemibold], colors.text, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        TextOut(dc, std::wstring(L"v") + kClickShadeVersionW + (elevated ? L"  ·  admin" : L""), {S(52), S(38), S(sidebarWidth), S(56)}, fonts[FontSmall], colors.muted, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        // Status block below the master switch; everything stays above the footer.
        const int block = StatusBlockHeight(heightDip);
        const int statusTop = heightDip - kFooterHeight - block;
        const int statusBottom = heightDip - kFooterHeight - 8;
        const COLORREF statusInk = StatusIsWarning() ? colors.warning : colors.text;
        if (block == kSidebarStatusTall) {
            const int previewTop = statusTop + 34;
            RoundBox(dc, {S(16), S(previewTop), S(16 + 48), S(previewTop + 40)}, RGB(48, 57, 70), RGB(48, 57, 70), S(8));
            RoundBox(dc, {S(72), S(previewTop), S(72 + 48), S(previewTop + 40)}, RGB(48, 57, 70), RGB(48, 57, 70), S(8));
            const auto preview = [&](HCURSOR cursor, SIZE source, int x) {
                const int box = S(30);
                int width = box, height = box;
                if (source.cx > source.cy) height = std::max(1, MulDiv(box, source.cy, source.cx));
                else if (source.cy > source.cx) width = std::max(1, MulDiv(box, source.cx, source.cy));
                DrawIconEx(dc, S(x) + (S(48) - width) / 2, S(previewTop) + (S(40) - height) / 2, cursor, width, height, 0, nullptr, DI_NORMAL);
            };
            if (normalPreview) preview(normalPreview, normalPreviewSize, 16);
            if (darkPreview) preview(darkPreview, darkPreviewSize, 72);
            TextOut(dc, L"Released", {S(16), S(previewTop + 42), S(64), S(previewTop + 58)}, fonts[FontSmall], colors.muted, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
            TextOut(dc, L"Pressed", {S(72), S(previewTop + 42), S(120), S(previewTop + 58)}, fonts[FontSmall], colors.muted, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
            TextOut(dc, StatusHeading(), {S(16), S(previewTop + 62), S(sidebarWidth - 12), S(previewTop + 80)}, fonts[FontSmall], statusInk,
                    DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            TextOut(dc, StatusDetail(), {S(16), S(previewTop + 82), S(sidebarWidth - 12), S(statusBottom)}, fonts[FontSmall], StatusIsWarning() ? colors.warning : colors.muted,
                    DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            TextOut(dc, StatusHeading(), {S(16), S(statusTop + 34), S(sidebarWidth - 12), S(statusBottom)}, fonts[FontSmall], statusInk,
                    DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    // Header.
    const auto& pageSpec = kPages[static_cast<std::size_t>(std::clamp(page, 0, static_cast<int>(kPages.size()) - 1))];
    TextOut(dc, pageSpec.title, {S(sidebarWidth + kContentMargin), S(14), S(widthDip - 150), S(42)}, fonts[FontTitle], colors.text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    TextOut(dc, pageSpec.subtitle, {S(sidebarWidth + kContentMargin), S(42), S(widthDip - 150), S(62)}, fonts[FontSmall], colors.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    const bool coreError = !startupError.empty() || !status.lastError.empty();
    const std::wstring pill = coreError ? L"ERROR" : settings.enabled && !status.pausedByRule ? L"ACTIVE" : L"PAUSED";
    const COLORREF pillInk = coreError ? colors.warning : settings.enabled && !status.pausedByRule ? colors.accentInk : colors.muted;
    const RECT pillRect{S(widthDip - 112), S(22), S(widthDip - 18), S(46)};
    RoundBox(dc, pillRect, Mix(colors.background, pillInk, .12), Mix(colors.background, pillInk, .35), S(12));
    TextOut(dc, pill, pillRect, fonts[FontSmall], pillInk, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    // Divider between header and content.
    const RECT divider{S(sidebarWidth), S(kHeaderHeight) - 1, client.right, S(kHeaderHeight)};
    HBRUSH line = CreateSolidBrush(colors.border); FillRect(dc, &divider, line); DeleteObject(line);
    // Footer with the spectrum line.
    const int footerTop = client.bottom - S(kFooterHeight);
    DrawSpectrum(dc, {0, footerTop, client.right, footerTop + 2});
    const std::wstring hint = page == kShortcutsPage ? L"Use Apply shortcuts to save bindings. Other changes save automatically." :
        hotkeyAvailable ? settings.hotkeyRestore + L" restores your cursors and pauses. Changes save automatically." :
        L"Changes save automatically. Closing keeps ClickShade in the tray.";
    TextOut(dc, hint, {S(16), footerTop + S(14), S(widthDip - 96 - 8 - 112 - 8 - 124 - 12), footerTop + S(36)}, fonts[FontSmall], colors.muted,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void App::PaintPanel(HDC dc) {
    RECT client{}; GetClientRect(panel, &client);
    FillRect(dc, &client, background);
    for (const auto& frame : paintFrames) {
        const RECT rect{S(frame.rect.left), S(frame.rect.top) - scroll, S(frame.rect.right), S(frame.rect.bottom) - scroll};
        if (rect.bottom < 0 || rect.top > client.bottom) continue;
        RoundBox(dc, rect, colors.card, colors.border, S(frame.radius));
    }
    for (const auto& text : paintTexts) {
        const RECT rect{S(text.rect.left), S(text.rect.top) - scroll, S(text.rect.right), S(text.rect.bottom) - scroll};
        if (rect.bottom < 0 || rect.top > client.bottom) continue;
        TextOut(dc, text.text, rect, fonts[static_cast<std::size_t>(text.font)], text.color, text.flags);
    }
    for (const auto& entry : editFrames) {
        const HWND control = controls.at(entry.first);
        if (!(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) || GetParent(control) != panel) continue;
        const auto& f = entry.second;
        const RECT rect{S(f.left), S(f.top) - scroll, S(f.right), S(f.bottom) - scroll};
        const bool focused = GetFocus() == control;
        RoundBox(dc, rect, colors.edit, focused ? colors.accent : colors.border, S(8));
    }
}

void App::DrawSlider(HWND child, HDC dc) {
    RECT rect{}, channel{}, thumb{}; GetClientRect(child, &rect); FillRect(dc, &rect, cardBrush);
    SendMessageW(child, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(child, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const bool enabled = IsWindowEnabled(child) != FALSE;
    const int hitRadius = std::max(1, static_cast<int>(std::min(thumb.right - thumb.left, thumb.bottom - thumb.top) - 1) / 2);
    const int radius = std::min({S(enabled ? 7 : 5), hitRadius, std::max(1, static_cast<int>(rect.bottom - rect.top) / 2 - 1)});
    const int center = std::clamp(static_cast<int>((thumb.top + thumb.bottom) / 2), radius + 1, std::max(radius + 1, static_cast<int>(rect.bottom) - radius - 1));
    const int minimum = static_cast<int>(rect.left) + radius + 1;
    const int maximum = std::max(minimum, static_cast<int>(rect.right) - radius - 1);
    const int x = std::clamp(static_cast<int>((thumb.left + thumb.right) / 2), minimum, maximum);
    const int left = std::clamp(static_cast<int>(channel.left), minimum, maximum);
    const int right = std::clamp(static_cast<int>(channel.right), left, maximum);
    const int half = std::max(2, S(2));
    HBRUSH track = CreateSolidBrush(colors.border), active = CreateSolidBrush(enabled ? colors.accent : colors.muted);
    HGDIOBJ oldBrush = SelectObject(dc, track), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, left, center - half, right + 1, center + half + 1, half * 2 + 1, half * 2 + 1);
    SelectObject(dc, active);
    if (x > left) RoundRect(dc, left, center - half, x + 1, center + half + 1, half * 2 + 1, half * 2 + 1);
    if (enabled) {
        // Soft halo under the knob.
        HBRUSH halo = CreateSolidBrush(Mix(colors.card, colors.accent, .28));
        SelectObject(dc, halo);
        Ellipse(dc, x - radius - S(3), center - radius - S(3), x + radius + S(4), center + radius + S(4));
        SelectObject(dc, active); DeleteObject(halo);
    }
    Ellipse(dc, x - radius, center - radius, x + radius + 1, center + radius + 1);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(track); DeleteObject(active);
    if (GetFocus() == child && !(SendMessageW(child, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) { InflateRect(&rect, -2, -2); DrawFocusRect(dc, &rect); }
}

void App::DrawButton(const DRAWITEMSTRUCT* item) {
    if (item->CtlType == ODT_COMBOBOX) {
        const bool selected = (item->itemState & ODS_SELECTED) != 0;
        HBRUSH brush = CreateSolidBrush(selected ? Mix(colors.edit, colors.accent, .20) : colors.edit);
        FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
        std::wstring label = L"Custom file / no library selection";
        const LRESULT length = SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0);
        if (length >= 0 && length < 32768) {
            std::vector<wchar_t> text(static_cast<std::size_t>(length) + 1);
            if (SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(text.data())) != CB_ERR)
                label = text.data();
        }
        RECT text = item->rcItem; text.left += S(8); text.right -= S(6);
        TextOut(item->hDC, label, text, fonts[FontNormal], (item->itemState & ODS_DISABLED) ? colors.muted : selected ? colors.accentInk : colors.text,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (item->itemState & ODS_FOCUS) { InflateRect(&text, -1, -2); DrawFocusRect(item->hDC, &text); }
        return;
    }
    const int id = static_cast<int>(item->CtlID);
    const bool navButton = id >= PageCursor && id <= PageShortcuts;
    const bool cardReset = id >= kCardResetBase && id < kCardResetBase + kCardResetCount;
    const bool sliderReset = id >= kResetOffset && id < kResetOffset + 1000 && Slider(id - kResetOffset) != nullptr;
    const bool primary = id == Hide;
    const bool selectedTab = (navButton && id == kPages[static_cast<std::size_t>(page)].id) ||
        id == CursorCurrent + settings.cursorStyle ||
        id == EaseLinear + settings.clickEasing ||
        id == TrailDots + settings.trailStyle ||
        id == ShakeOff + settings.shakeStyle ||
        id == HighlightFilledStyle + settings.highlightStyle ||
        id == ClickRingStyle + settings.clickStyle ||
        id == (settings.magnifierShape ? MagnifierSquare : MagnifierCircle) ||
        id == (settings.magnifierPosition ? MagnifierBeside : MagnifierCentered) ||
        id == (settings.keyDisplayMode ? KeyDisplayAll : KeyDisplayShortcuts) ||
        id == KeyDisplayBottom + settings.keyDisplayPosition ||
        id == (settings.uiDarkMode ? ThemeDarkMode : ThemeLightMode);
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool onWindow = navButton || id == Hide || id == Refresh || id == Quit;
    // Owner draw owns the corner pixels too. Clear the native button face
    // before its rounded inset, especially after a DPI/zoom transition.
    FillRect(item->hDC, &item->rcItem, navButton ? sidebarBrush : onWindow ? background : cardBrush);
    RECT rect = item->rcItem;
    if (navButton) {
        const bool compact = sidebarWidth == kSidebarCompactWidth;
        if (selectedTab || pressed) {
            RoundBox(item->hDC, rect, Mix(colors.sidebar, colors.accent, pressed ? .22 : .14), Mix(colors.sidebar, colors.accent, pressed ? .22 : .14), S(9));
            if (selectedTab) {
                HBRUSH bar = CreateSolidBrush(colors.accent);
                const RECT accent{rect.left, rect.top + S(9), rect.left + S(3), rect.bottom - S(9)};
                FillRect(item->hDC, &accent, bar); DeleteObject(bar);
            }
        }
        const COLORREF ink = selectedTab ? colors.accentInk : colors.muted;
        DrawNavGlyph(item->hDC, id - PageCursor, rect.left + S(compact ? 12 : 14), rect.top + (rect.bottom - rect.top - S(18)) / 2, S(18), ink);
        if (!compact) {
            wchar_t label[64]{}; GetWindowTextW(item->hwndItem, label, 64);
            RECT text = rect; text.left += S(42);
            TextOut(item->hDC, label, text, fonts[FontNormal], selectedTab ? colors.text : colors.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (item->itemState & ODS_FOCUS) { InflateRect(&rect, -3, -3); DrawFocusRect(item->hDC, &rect); }
        return;
    }
    if (cardReset || sliderReset) {
        const COLORREF ink = disabled ? Mix(colors.card, colors.muted, .45) : pressed ? colors.accentInk : colors.muted;
        if (!disabled && pressed) RoundBox(item->hDC, rect, Mix(colors.card, colors.accent, .14), Mix(colors.card, colors.accent, .14), S(7));
        if (sliderReset) {
            // Counter-clockwise arrow glyph.
            HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), ink);
            HGDIOBJ oldPen = SelectObject(item->hDC, pen), oldBrush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
            const int cx = (rect.left + rect.right) / 2, cy = (rect.top + rect.bottom) / 2, r = S(6);
            Arc(item->hDC, cx - r, cy - r, cx + r, cy + r, cx - r, cy - S(2), cx - S(1), cy - r);
            HBRUSH head = CreateSolidBrush(ink); SelectObject(item->hDC, head); SelectObject(item->hDC, GetStockObject(NULL_PEN));
            POINT tip[]{{cx - r - S(3), cy - S(4)}, {cx - r + S(3), cy - S(4)}, {cx - r, cy + S(1)}};
            Polygon(item->hDC, tip, 3);
            SelectObject(item->hDC, oldPen); SelectObject(item->hDC, oldBrush); DeleteObject(pen); DeleteObject(head);
        } else {
            TextOut(item->hDC, L"Reset", rect, fonts[FontSmall], ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (item->itemState & ODS_FOCUS) { InflateRect(&rect, -2, -2); DrawFocusRect(item->hDC, &rect); }
        return;
    }
    const COLORREF fill = primary ? (pressed ? Mix(colors.accent, colors.background, .16) : colors.accent)
                                 : selectedTab ? Mix(colors.card, colors.accent, .18) :
                                   Mix(onWindow ? colors.background : colors.card, colors.text, pressed ? .13 : .045);
    RoundBox(item->hDC, rect, fill, primary ? fill : selectedTab ? Mix(colors.border, colors.accent, .35) : colors.border, S(9));
    wchar_t label[128]{}; GetWindowTextW(item->hwndItem, label, 128);
    const COLORREF ink = disabled ? colors.muted : primary ? colors.accentText : selectedTab ? colors.accentInk : colors.text;
    if (id == HighlightColor || id == TrailColor || id == ColorLeft || id == ColorRight || id == ColorMiddle || id == ThemeBackground || id == ThemeAccent ||
        id == MagnifierBorderColor || id == KeyDisplayColor || id == CursorFillColor || id == CursorOutlineColor) {
        const COLORREF color = id == HighlightColor ? settings.highlightColor : id == TrailColor ? settings.trailColor :
            id == ColorLeft ? settings.leftClickColor : id == ColorRight ? settings.rightClickColor : id == ColorMiddle ? settings.middleClickColor :
            id == MagnifierBorderColor ? settings.magnifierBorderColor : id == KeyDisplayColor ? settings.keyDisplayColor :
            id == CursorFillColor ? settings.cursorFillColor : id == CursorOutlineColor ? settings.cursorOutlineColor :
            id == ThemeBackground ? settings.uiBackground : settings.uiAccent;
        const LONG center = (rect.top + rect.bottom) / 2;
        RoundBox(item->hDC, {rect.left + S(10), center - S(7), rect.left + S(24), center + S(7)}, color, Mix(color, colors.text, .25), S(4));
        rect.left += S(22);
    }
    if (id >= TrailDots && id <= TrailFineSparkle) {
        const int x = rect.left + S(18), y = (rect.top + rect.bottom) / 2;
        const COLORREF glyphInk = selectedTab ? colors.accentInk : colors.muted;
        HBRUSH marker = CreateSolidBrush(glyphInk); HPEN outline = CreatePen(PS_SOLID, S(2), glyphInk);
        HGDIOBJ previousBrush = SelectObject(item->hDC, id == TrailRings ? GetStockObject(NULL_BRUSH) : marker);
        HGDIOBJ previousPen = SelectObject(item->hDC, outline);
        if (id == TrailSparkles || id == TrailFineSparkle) {
            const int core = id == TrailFineSparkle ? 1 : 2;
            POINT points[]{{x, y - S(6)}, {x + S(core), y - S(core)}, {x + S(6), y}, {x + S(core), y + S(core)},
                           {x, y + S(6)}, {x - S(core), y + S(core)}, {x - S(6), y}, {x - S(core), y - S(core)}};
            Polygon(item->hDC, points, 8);
        } else if (id == TrailComet) {
            POINT tail[]{{x - S(7), y + S(4)}, {x + S(2), y - S(5)}, {x + S(5), y - S(1)}};
            Polygon(item->hDC, tail, 3);
            Ellipse(item->hDC, x + S(1), y - S(5), x + S(6), y);
        } else Ellipse(item->hDC, x - S(4), y - S(4), x + S(4), y + S(4));
        SelectObject(item->hDC, previousBrush); SelectObject(item->hDC, previousPen);
        DeleteObject(marker); DeleteObject(outline); rect.left += S(22);
    }
    if (id >= ThemeMint && id <= ThemeLight) {
        constexpr COLORREF swatches[] = {RGB(120, 226, 194), RGB(107, 176, 255), RGB(241, 151, 190), RGB(54, 113, 215)};
        HBRUSH swatch = CreateSolidBrush(swatches[id - ThemeMint]);
        const RECT strip{rect.left + S(12), rect.bottom - S(8), rect.right - S(12), rect.bottom - S(5)};
        FillRect(item->hDC, &strip, swatch); DeleteObject(swatch);
        rect.bottom -= S(6);
    }
    TextOut(item->hDC, label, rect, fonts[FontNormal], ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (item->itemState & ODS_FOCUS) { InflateRect(&rect, -4, -4); DrawFocusRect(item->hDC, &rect); }
}

void App::DrawToggle(HWND child, HDC dc) {
    RECT rect{}; GetClientRect(child, &rect);
    const int id = GetDlgCtrlID(child);
    const bool onWindow = id == Enable;
    FillRect(dc, &rect, onWindow ? sidebarBrush : cardBrush);
    const bool compact = (id >= Left && id <= Side2);
    const bool checked = SendMessageW(child, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool enabled = IsWindowEnabled(child) != FALSE;
    const int height = S(18), width = S(compact ? 18 : 32);
    const int top = (rect.bottom - height) / 2;
    COLORREF track = checked ? colors.accent : colors.edit;
    if (!enabled) track = Mix(track, onWindow ? colors.sidebar : colors.card, .6);
    RoundBox(dc, {0, top, width, top + height}, track, checked ? track : colors.border, S(compact ? 5 : 18));
    if (compact && checked) {
        HPEN check = CreatePen(PS_SOLID, S(2), colors.accentText);
        HGDIOBJ oldPen = SelectObject(dc, check);
        MoveToEx(dc, S(4), top + S(9), nullptr); LineTo(dc, S(8), top + S(13)); LineTo(dc, S(14), top + S(5));
        SelectObject(dc, oldPen); DeleteObject(check);
    } else if (!compact) {
        const COLORREF knob = checked ? colors.accentText : colors.muted;
        HBRUSH thumb = CreateSolidBrush(knob);
        HGDIOBJ oldBrush = SelectObject(dc, thumb), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        const int left = checked ? width - height + S(3) : S(3);
        Ellipse(dc, left, top + S(3), left + height - S(6), top + height - S(3));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(thumb);
    }
    const bool iconOnly = onWindow && sidebarWidth == kSidebarCompactWidth;
    if (!iconOnly) {
        wchar_t label[256]{}; GetWindowTextW(child, label, 256);
        RECT labelRect = rect; labelRect.left = width + S(10);
        TextOut(dc, label, labelRect, fonts[FontNormal], enabled ? colors.text : colors.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (GetFocus() == child && !(SendMessageW(child, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            InflateRect(&labelRect, -1, -2); DrawFocusRect(dc, &labelRect);
        }
    } else if (GetFocus() == child) {
        RECT focus{0, top - 2, width + 2, top + height + 2}; DrawFocusRect(dc, &focus);
    }
}

} // namespace clickshade::ui
