#include "visual_effects.h"
#include "frame_pacing.h"
#include "key_display.h"
#include "win_event_listener.h"

#include <algorithm>
#include <climits>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace clickshade {
namespace {
constexpr wchar_t kControllerClass[] = L"ClickShade.VisualController.v1";
constexpr wchar_t kOverlayClass[] = L"ClickShade.VisualOverlay.v1";
constexpr wchar_t kLensClass[] = L"ClickShade.VisualLens.v1";
constexpr UINT kApplySettings = WM_APP + 121;
constexpr UINT kApplyAllowed = WM_APP + 122;
constexpr UINT kPointerKick = WM_APP + 123;
constexpr UINT kClick = WM_APP + 124;
constexpr UINT kMonitorChanged = WM_APP + 125;
constexpr UINT kStackChanged = WM_APP + 126;
constexpr UINT kKeyEvent = WM_APP + 127;
constexpr DWORD kHighResolutionTimer = 0x00000002;
constexpr std::size_t kParticleCount = 8;
constexpr std::size_t kClickQueueSize = 32;
constexpr std::size_t kTrailCount = 32;
constexpr std::size_t kKeyRowLimit = 3; // Rows on the key display; the oldest leaves first.
constexpr std::size_t kTypedLimit = 120; // Characters kept in one typed row.
constexpr double kPi = 3.14159265358979323846;
constexpr double kBreathPeriodSeconds = 3.2;
constexpr int kKeyFadeMs = 350;
constexpr unsigned kLensIdleRate = 30; // Live lens redraws per second while the pointer rests.
// Magnification API values (magnification.h is not included: the API is
// loaded at run time so machines without it keep every other effect).
constexpr DWORD kMagnifierShowCursor = 0x0001;
constexpr DWORD kMagnifierFilterExclude = 0;
// MagSetWindowFilterList fails with ERROR_NOT_SUPPORTED above 24 windows.
constexpr std::size_t kMagnifierFilterLimit = 24;
// The lens host shows its magnifier child through a color key; this exact
// color never appears in real content often enough to matter.
constexpr COLORREF kLensKey = RGB(253, 2, 254);

struct Config {
    bool enabled = true;
    bool highlight = false;
    int highlightStyle = kHighlightFilled;
    int highlightRadius = 24;
    int highlightOpacity = 35;
    int highlightThickness = 3;
    COLORREF highlightColor = RGB(255, 214, 64);
    bool breathing = false;
    int idleHideMs = 0;
    int spotlightDim = 55;
    int spotlightRadius = 140;
    bool trail = false;
    int trailStyle = 0;
    int trailRadius = 5;
    int trailOpacity = 45;
    int trailDurationMs = 220;
    COLORREF trailColor = RGB(120, 226, 194);
    bool clicks = false;
    unsigned buttons = 3;
    int clickStyle = kClickRing;
    int clickRadius = 42;
    int durationMs = 360;
    int clickOpacity = 80;
    int clickThickness = 3;
    int easing = 1;
    unsigned refreshRate = 0;
    COLORREF leftColor = RGB(0, 200, 255);
    COLORREF rightColor = RGB(255, 49, 49);
    COLORREF middleColor = RGB(57, 255, 20);
    bool captureVisible = true;
    bool magnifier = false;
    int magnifierZoom = 200;
    int magnifierSize = 220;
    int magnifierShape = 0;
    int magnifierPosition = 0;
    COLORREF magnifierBorder = RGB(255, 255, 255);
    bool magnifierFrame = true;
    bool keyDisplay = false;
    bool keyMouse = false;
    int keyPosition = 0;
    int keySize = 100;
    int keyDurationMs = 1500;
    int keyOpacity = 90;
    COLORREF keyColor = RGB(28, 31, 38);
};

Config MakeConfig(const Settings& source) noexcept {
    Config result;
    result.enabled = source.enabled;
    result.highlight = source.highlightEnabled;
    result.highlightStyle = std::clamp(source.highlightStyle, 0, kHighlightStyleCount - 1);
    result.highlightRadius = std::clamp(source.highlightRadius, 4, 128);
    result.highlightOpacity = std::clamp(source.highlightOpacity, 0, 100);
    result.highlightThickness = std::clamp(source.highlightThickness, 1, 12);
    result.highlightColor = source.highlightColor & 0x00ffffff;
    result.breathing = source.highlightBreathing;
    result.idleHideMs = source.highlightIdleHideMs <= 0 ? 0 : std::clamp(source.highlightIdleHideMs, 500, 10000);
    result.spotlightDim = std::clamp(source.spotlightDim, 10, 90);
    result.spotlightRadius = std::clamp(source.spotlightRadius, 40, 400);
    result.trail = source.trailEnabled;
    result.trailStyle = std::clamp(source.trailStyle, 0, 4);
    result.trailRadius = std::clamp(source.trailRadius, 2, 16);
    result.trailOpacity = std::clamp(source.trailOpacity, 0, 100);
    result.trailDurationMs = std::clamp(source.trailDurationMs, 80, 700);
    result.trailColor = source.trailColor & 0x00ffffff;
    result.clicks = source.clickEffectsEnabled;
    result.buttons = (source.leftClickEnabled ? 1u : 0u) | (source.rightClickEnabled ? 2u : 0u) |
                     (source.middleClickEnabled ? 4u : 0u);
    result.clickStyle = std::clamp(source.clickStyle, 0, kClickStyleCount - 1);
    result.clickRadius = std::clamp(source.clickRadius, 8, 160);
    result.durationMs = std::clamp(source.clickDurationMs, 80, 1500);
    result.clickOpacity = std::clamp(source.clickOpacity, 0, 100);
    result.clickThickness = std::clamp(source.clickThickness, 1, 12);
    result.easing = std::clamp(source.clickEasing, 0, 2);
    result.refreshRate = source.visualRefreshRate <= 0 ? 0u :
        static_cast<unsigned>(std::clamp(source.visualRefreshRate, 30, 360));
    result.leftColor = source.leftClickColor & 0x00ffffff;
    result.rightColor = source.rightClickColor & 0x00ffffff;
    result.middleColor = source.middleClickColor & 0x00ffffff;
    result.captureVisible = source.captureVisible;
    result.magnifier = source.magnifierEnabled;
    result.magnifierZoom = std::clamp(source.magnifierZoom, 125, 800);
    result.magnifierSize = std::clamp(source.magnifierSize, 80, 600);
    result.magnifierShape = std::clamp(source.magnifierShape, 0, 1);
    result.magnifierPosition = std::clamp(source.magnifierPosition, 0, 1);
    result.magnifierBorder = source.magnifierBorderColor & 0x00ffffff;
    result.magnifierFrame = source.magnifierFrame;
    result.keyDisplay = source.keyDisplayEnabled;
    result.keyMouse = source.keyDisplayMouse;
    result.keyPosition = std::clamp(source.keyDisplayPosition, 0, 3);
    result.keySize = std::clamp(source.keyDisplaySize, 60, 250);
    result.keyDurationMs = std::clamp(source.keyDisplayDurationMs, 300, 6000);
    result.keyOpacity = std::clamp(source.keyDisplayOpacity, 20, 100);
    result.keyColor = source.keyDisplayColor & 0x00ffffff;
    return result;
}

bool MagnifierAppearanceChanged(const Config& a, const Config& b) noexcept {
    return a.magnifierZoom != b.magnifierZoom || a.magnifierSize != b.magnifierSize ||
        a.magnifierShape != b.magnifierShape || a.magnifierPosition != b.magnifierPosition ||
        a.magnifierBorder != b.magnifierBorder || a.magnifierFrame != b.magnifierFrame;
}

bool KeyAppearanceChanged(const Config& a, const Config& b) noexcept {
    return a.keyPosition != b.keyPosition || a.keySize != b.keySize ||
        a.keyOpacity != b.keyOpacity || a.keyColor != b.keyColor;
}

// Alpha of the spotlight's dimmed area.
BYTE ShadeAlpha(int dimPercent) noexcept {
    return static_cast<BYTE>(std::clamp(255 * dimPercent / 100, 0, 255));
}

std::wstring Failure(const wchar_t* operation, DWORD code) {
    if (!code) code = ERROR_GEN_FAILURE;
    return std::wstring(operation) + L" (Windows error " + std::to_wstring(code) + L").";
}

bool VisibleCursor(POINT& point) noexcept {
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING) ||
        (info.flags & CURSOR_SUPPRESSED) || !info.hCursor) return false;
    point = info.ptScreenPos;
    return true;
}

LONGLONG CounterNow() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

bool HighlightAppearanceChanged(const Config& first, const Config& second) noexcept {
    return first.highlightStyle != second.highlightStyle ||
           first.highlightRadius != second.highlightRadius ||
           first.highlightOpacity != second.highlightOpacity ||
           first.highlightThickness != second.highlightThickness ||
           first.highlightColor != second.highlightColor ||
           first.spotlightDim != second.spotlightDim ||
           first.spotlightRadius != second.spotlightRadius;
}

bool TrailAppearanceChanged(const Config& a, const Config& b) noexcept {
    return a.trail != b.trail || a.trailStyle != b.trailStyle ||
        a.trailRadius != b.trailRadius || a.trailOpacity != b.trailOpacity ||
        a.trailDurationMs != b.trailDurationMs || a.trailColor != b.trailColor;
}

// WDA_EXCLUDEFROMCAPTURE needs Windows 10 version 2004; older builds reject
// it, which is reported once and otherwise harmless.
bool ApplyCaptureAffinity(HWND window, bool captureVisible) noexcept {
    if (!window) return true;
    return SetWindowDisplayAffinity(window, captureVisible ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE) != FALSE ||
           captureVisible;
}

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
            reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        break;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: return TRUE;
    case WM_DPICHANGED:
        // Geometry is computed in physical pixels from the cursor's monitor.
        // Accepting the suggested window rectangle would scale it a second time.
        if (const HWND controller = reinterpret_cast<HWND>(GetWindowLongPtrW(window, GWLP_USERDATA)))
            PostMessageW(controller, kMonitorChanged, 0, 0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// Magnifier host: a color-keyed layered window whose only visible content is
// the round (or rounded) Magnification API child window inside it.
LRESULT CALLBACK LensProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: return TRUE;
    case WM_DPICHANGED: return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        HBRUSH key = CreateSolidBrush(kLensKey);
        FillRect(dc, &paint.rcPaint, key);
        DeleteObject(key);
        EndPaint(window, &paint);
        return 0;
    }
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}

// MAGTRANSFORM: a 3x3 matrix; the zoom sits on the diagonal.
struct MagnifierTransform { float v[3][3]; };

// Run-time binding to magnification.dll. Nothing links against it, so a
// Windows installation without the API keeps every other effect working.
struct MagnificationApi {
    HMODULE library = nullptr;
    BOOL (WINAPI* initialize)() = nullptr;
    BOOL (WINAPI* uninitialize)() = nullptr;
    BOOL (WINAPI* setSource)(HWND, RECT) = nullptr;
    BOOL (WINAPI* setFilter)(HWND, DWORD, int, HWND*) = nullptr;
    BOOL (WINAPI* setTransform)(HWND, MagnifierTransform*) = nullptr;
    // Optional: Windows 10 and later smooth the enlarged image like the
    // system Magnifier's "smooth edges" option. Older systems scale plainly.
    BOOL (WINAPI* setSmoothing)(HWND, BOOL) = nullptr;
    bool initialized = false;

    template <class Function>
    static bool Resolve(HMODULE module, const char* name, Function& target) noexcept {
        const FARPROC address = GetProcAddress(module, name);
        if (!address) return false;
        static_assert(sizeof(address) == sizeof(target), "Windows function pointer size");
        std::memcpy(&target, &address, sizeof(address));
        return true;
    }

    bool Load(std::wstring& error) {
        if (initialized) return true;
        if (!library) library = LoadLibraryExW(L"magnification.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) { error = Failure(L"The Windows Magnification API is not installed", GetLastError()); return false; }
        if (!Resolve(library, "MagInitialize", initialize) || !Resolve(library, "MagUninitialize", uninitialize) ||
            !Resolve(library, "MagSetWindowSource", setSource) || !Resolve(library, "MagSetWindowFilterList", setFilter) ||
            !Resolve(library, "MagSetWindowTransform", setTransform)) {
            error = L"The Windows Magnification API is incomplete on this system.";
            return false;
        }
        if (!Resolve(library, "MagSetLensUseBitmapSmoothing", setSmoothing)) setSmoothing = nullptr;
        if (!initialize()) { error = Failure(L"Windows could not start its magnifier service", GetLastError()); return false; }
        initialized = true;
        return true;
    }

    void Unload() noexcept {
        if (initialized && uninitialize) uninitialize();
        initialized = false;
        if (library) FreeLibrary(library);
        library = nullptr;
        initialize = uninitialize = nullptr;
        setSource = nullptr;
        setFilter = nullptr;
        setTransform = nullptr;
        setSmoothing = nullptr;
    }
};

// The first visible window of another thread above `window` that overlaps
// `target`. Usually this inspects only ClickShade's own few overlays; the walk
// is bounded because other processes can destroy or reorder windows meanwhile.
HWND ForeignWindowAbove(HWND window, const RECT& target) noexcept {
    HWND above = GetWindow(window, GW_HWNDPREV);
    for (unsigned count = 0; above && count < 128; ++count) {
        const HWND next = GetWindow(above, GW_HWNDPREV);
        if (GetWindowThreadProcessId(above, nullptr) != GetCurrentThreadId() &&
            IsWindowVisible(above) && !IsIconic(above)) {
            RECT bounds{}, intersection{};
            if (GetWindowRect(above, &bounds) && IntersectRect(&intersection, &target, &bounds))
                return above;
        }
        if (next == above || next == window) break;
        above = next;
    }
    return nullptr;
}

inline std::uint32_t Premultiplied(unsigned red, unsigned green, unsigned blue, unsigned alpha) noexcept {
    return (alpha << 24) | (((red * alpha + 127) / 255) << 16) |
           (((green * alpha + 127) / 255) << 8) | ((blue * alpha + 127) / 255);
}

struct Surface {
    struct RowSpans { int begin = 0, end = 0, secondBegin = 0, secondEnd = 0; };
    HWND window = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ originalBitmap = nullptr;
    std::uint32_t* pixels = nullptr;
    int size = 0;   // Square surfaces: side length used by the shape painters.
    int width = 0, height = 0; // Actual bitmap dimensions (equal to size for squares).
    bool visible = false;
    bool positioned = false;
    POINT lastCenter{};
    POINT lastOrigin{}; // Window top-left of the last placement.
    HWND resistedBy = nullptr;
    ULONGLONG retryStackAfter = 0;
    std::vector<RowSpans> paintedRows;
    BYTE lastFade = 255;

    Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    ~Surface() { Reset(); }

    void Hide() noexcept {
        if (window && visible) ShowWindow(window, SW_HIDE);
        visible = false;
        positioned = false;
        resistedBy = nullptr;
        retryStackAfter = 0;
    }

    void Reset() noexcept {
        if (window) DestroyWindow(window);
        window = nullptr;
        visible = false;
        positioned = false;
        if (dc && originalBitmap) SelectObject(dc, originalBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr;
        bitmap = nullptr;
        originalBitmap = nullptr;
        pixels = nullptr;
        size = width = height = 0;
        paintedRows.clear();
    }

    bool Prepare(int requestedSize, HWND controller, bool captureVisible, std::wstring& error) {
        return PrepareRect(requestedSize, requestedSize, controller, captureVisible, error, L"ClickShade pointer effect");
    }

    bool PrepareRect(int requestedWidth, int requestedHeight, HWND controller, bool captureVisible,
                     std::wstring& error, const wchar_t* title) {
        requestedWidth = std::max(1, requestedWidth);
        requestedHeight = std::max(1, requestedHeight);
        if (!window) {
            window = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                    WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                kOverlayClass, title, WS_POPUP,
                0, 0, requestedWidth, requestedHeight, nullptr, nullptr,
                GetModuleHandleW(nullptr), controller);
            if (!window) {
                const DWORD code = GetLastError();
                error = Failure(L"Could not create a pointer effect window", code);
                return false;
            }
            ApplyCaptureAffinity(window, captureVisible);
        }
        if (bitmap && requestedWidth == width && requestedHeight == height) return true;
        std::vector<RowSpans> nextRows(static_cast<std::size_t>(requestedHeight));
        if (!dc) {
            dc = CreateCompatibleDC(nullptr);
            if (!dc) {
                const DWORD code = GetLastError();
                error = Failure(L"Could not create a pointer effect graphics context", code);
                return false;
            }
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = requestedWidth;
        info.bmiHeader.biHeight = -requestedHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* address = nullptr;
        HBITMAP next = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                        &address, nullptr, 0);
        if (!next || !address) {
            const DWORD code = GetLastError();
            if (next) DeleteObject(next);
            error = Failure(L"Could not allocate pointer effect pixels", code);
            return false;
        }
        HGDIOBJ old = SelectObject(dc, next);
        if (!old || old == HGDI_ERROR) {
            const DWORD code = GetLastError();
            DeleteObject(next);
            error = Failure(L"Could not select the pointer effect bitmap", code);
            return false;
        }
        if (!originalBitmap) originalBitmap = old;
        if (bitmap) DeleteObject(bitmap);
        bitmap = next;
        pixels = static_cast<std::uint32_t*>(address);
        width = requestedWidth;
        height = requestedHeight;
        size = requestedWidth == requestedHeight ? requestedWidth : 0;
        paintedRows = std::move(nextRows);
        std::memset(pixels, 0, static_cast<std::size_t>(width) * height * 4);
        return true;
    }

    void ClearPainted() noexcept {
        if (!pixels || width <= 0) return;
        for (int y = 0; y < height; ++y) {
            const auto& spans = paintedRows[static_cast<std::size_t>(y)];
            auto* row = pixels + static_cast<std::size_t>(y) * width;
            std::fill(row + spans.begin, row + spans.end, 0u);
            std::fill(row + spans.secondBegin, row + spans.secondEnd, 0u);
            paintedRows[static_cast<std::size_t>(y)] = {};
        }
    }

    void MarkAllPainted() noexcept {
        for (int y = 0; y < height; ++y) paintedRows[static_cast<std::size_t>(y)] = {0, width, 0, 0};
    }

    void Clear() noexcept {
        if (!pixels || width <= 0) return;
        std::memset(pixels, 0, static_cast<std::size_t>(width) * height * 4);
        for (auto& spans : paintedRows) spans = {};
    }

    void DrawCircle(double radius, double lineWidth, bool filled,
                    COLORREF color, int opacity) noexcept {
        if (!pixels || size <= 0) return;
        // Clear only the spans written by the previous circle. Outline effects
        // therefore touch their narrow rings instead of the whole square DIB.
        ClearPainted();
        const double center = (static_cast<double>(size) - 1.0) * 0.5;
        const double inner = std::max(0.0, radius - lineWidth);
        const double outerFull = std::max(0.0, radius - 0.5);
        const double outerEmpty = radius + 0.5;
        const double innerFull = std::max(0.0, inner - 0.5);
        const double innerEmpty = inner + 0.5;
        const double maxAlpha = 255.0 * static_cast<double>(opacity) / 100.0;
        const unsigned red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
        const auto solid = Premultiplied(red, green, blue, static_cast<unsigned>(maxAlpha + 0.5));
        const double outerSquared = outerEmpty * outerEmpty;
        const double fullSquared = outerFull * outerFull;
        const double innerSquared = innerFull * innerFull;
        const int first = std::max(0, static_cast<int>(std::ceil(center - outerEmpty)));
        const int last = std::min(size - 1, static_cast<int>(std::floor(center + outerEmpty)));
        for (int y = first; y <= last; ++y) {
            const double dy = static_cast<double>(y) - center;
            const double dy2 = dy * dy;
            if (dy2 >= outerSquared) continue;
            const double extent = std::sqrt(outerSquared - dy2);
            const int left = std::max(0, static_cast<int>(std::ceil(center - extent)));
            const int right = std::min(size, static_cast<int>(std::floor(center + extent)) + 1);
            auto* row = pixels + static_cast<std::size_t>(y) * size;
            const auto edge = [&](int begin, int end) {
                for (int x = begin; x < end; ++x) {
                    const double dx = static_cast<double>(x) - center;
                    const double distance2 = dx * dx + dy2;
                    double coverage = 1.0;
                    if (distance2 > fullSquared)
                        coverage = std::clamp(outerEmpty - std::sqrt(distance2), 0.0, 1.0);
                    if (!filled && inner > 0.0 && distance2 < innerEmpty * innerEmpty)
                        coverage -= std::clamp(innerEmpty - std::sqrt(distance2), 0.0, 1.0);
                    if (coverage >= 1.0) row[x] = solid;
                    else if (coverage > 0.0) row[x] = Premultiplied(red, green, blue, static_cast<unsigned>(
                        std::clamp(maxAlpha * coverage + 0.5, 0.0, 255.0)));
                }
            };
            auto& spans = paintedRows[static_cast<std::size_t>(y)];
            if (filled) {
                spans = {left, right, 0, 0};
                if (dy2 < fullSquared) {
                    const double fullExtent = std::sqrt(fullSquared - dy2);
                    const int fullLeft = std::clamp(static_cast<int>(std::ceil(center - fullExtent)), left, right);
                    const int fullRight = std::clamp(static_cast<int>(std::floor(center + fullExtent)) + 1, fullLeft, right);
                    std::fill(row + fullLeft, row + fullRight, solid);
                    edge(left, fullLeft);
                    edge(fullRight, right);
                } else edge(left, right);
            } else if (dy2 < innerSquared) {
                const double emptyExtent = std::sqrt(innerSquared - dy2);
                const int emptyLeft = std::clamp(static_cast<int>(std::ceil(center - emptyExtent)), left, right);
                const int emptyRight = std::clamp(static_cast<int>(std::floor(center + emptyExtent)) + 1, emptyLeft, right);
                spans = {left, emptyLeft, emptyRight, right};
                edge(left, emptyLeft);
                edge(emptyRight, right);
            } else {
                spans = {left, right, 0, 0};
                edge(left, right);
            }
        }
    }

    // Visits every pixel whose center lies between innerRadius and outerRadius
    // from the surface center, row by row, and records the painted spans. The
    // callback returns the alpha (0..255) for that pixel.
    template <class Alpha>
    void PaintAnnulus(double innerRadius, double outerRadius, COLORREF color, Alpha alphaOf) noexcept {
        if (!pixels || size <= 0) return;
        ClearPainted();
        const double center = (static_cast<double>(size) - 1.0) * 0.5;
        const unsigned red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
        innerRadius = std::max(0.0, innerRadius);
        const double outerSquared = outerRadius * outerRadius;
        const double innerSquared = innerRadius * innerRadius;
        const int first = std::max(0, static_cast<int>(std::ceil(center - outerRadius)));
        const int last = std::min(size - 1, static_cast<int>(std::floor(center + outerRadius)));
        for (int y = first; y <= last; ++y) {
            const double dy = static_cast<double>(y) - center;
            const double dy2 = dy * dy;
            if (dy2 > outerSquared) continue;
            const double extent = std::sqrt(outerSquared - dy2);
            const int left = std::max(0, static_cast<int>(std::ceil(center - extent)));
            const int right = std::min(size, static_cast<int>(std::floor(center + extent)) + 1);
            auto* row = pixels + static_cast<std::size_t>(y) * size;
            const auto paint = [&](int begin, int end) {
                for (int x = begin; x < end; ++x) {
                    const double dx = static_cast<double>(x) - center;
                    const unsigned alpha = alphaOf(dx, dy, std::sqrt(dx * dx + dy2));
                    row[x] = alpha ? Premultiplied(red, green, blue, std::min(255u, alpha)) : 0u;
                }
            };
            auto& spans = paintedRows[static_cast<std::size_t>(y)];
            if (innerRadius > 0.0 && dy2 < innerSquared) {
                const double hole = std::sqrt(innerSquared - dy2);
                const int holeLeft = std::clamp(static_cast<int>(std::ceil(center - hole)), left, right);
                const int holeRight = std::clamp(static_cast<int>(std::floor(center + hole)) + 1, holeLeft, right);
                spans = {left, holeLeft, holeRight, right};
                paint(left, holeLeft);
                paint(holeRight, right);
            } else {
                spans = {left, right, 0, 0};
                paint(left, right);
            }
        }
    }

    // Soft radial glow: full in the middle, fading to nothing at the radius.
    void DrawGlow(double radius, COLORREF color, int opacity) noexcept {
        const double maxAlpha = 255.0 * opacity / 100.0;
        PaintAnnulus(0.0, radius + 0.5, color, [&](double, double, double distance) {
            const double t = std::clamp(1.0 - distance / radius, 0.0, 1.0);
            return static_cast<unsigned>(maxAlpha * t * t * (3.0 - 2.0 * t) + 0.5);
        });
    }

    // Four arms with a clear center so the pointer tip stays unobstructed.
    void DrawCrosshair(double radius, double lineWidth, COLORREF color, int opacity) noexcept {
        const double maxAlpha = 255.0 * opacity / 100.0;
        const double gap = std::max(2.0, radius * 0.32);
        const double half = std::max(0.5, lineWidth * 0.5);
        PaintAnnulus(std::max(0.0, gap - half - 1.0), radius + half + 1.0, color, [&](double dx, double dy, double) {
            const double ax = std::abs(dx), ay = std::abs(dy);
            const auto arm = [&](double along, double across) {
                const double lengthCoverage = std::clamp(std::min(along - gap, radius - along) + 0.5, 0.0, 1.0);
                const double widthCoverage = std::clamp(half - across + 0.5, 0.0, 1.0);
                return lengthCoverage * widthCoverage;
            };
            const double coverage = std::max(arm(ax, ay), arm(ay, ax));
            return static_cast<unsigned>(maxAlpha * coverage + 0.5);
        });
    }

    // Three concentric rings that trail the leading edge.
    void DrawRipple(double radius, double lineWidth, COLORREF color, int opacity) noexcept {
        const double maxAlpha = 255.0 * opacity / 100.0;
        const double half = std::max(0.5, lineWidth * 0.5);
        const double rings[3] = {radius, radius * 0.70, radius * 0.42};
        const double weights[3] = {1.0, 0.62, 0.36};
        PaintAnnulus(std::max(0.0, rings[2] - half - 1.0), radius + half + 1.0, color, [&](double, double, double distance) {
            double coverage = 0.0;
            for (unsigned i = 0; i < 3; ++i)
                coverage = std::max(coverage, weights[i] * std::clamp(half - std::abs(distance - rings[i]) + 0.5, 0.0, 1.0));
            return static_cast<unsigned>(maxAlpha * coverage + 0.5);
        });
    }

    // Twelve short rays flying outward; progress moves them from the center
    // to the configured radius while they shorten.
    void DrawBurst(double radius, double lineWidth, COLORREF color, int opacity, double progress) noexcept {
        const double maxAlpha = 255.0 * opacity / 100.0;
        const double half = std::max(0.75, lineWidth * 0.5);
        const double head = radius * (0.18 + 0.82 * progress);
        const double length = std::max(2.0, radius * 0.30 * (1.0 - 0.6 * progress));
        const double tail = std::max(0.0, head - length);
        constexpr unsigned rays = 12;
        PaintAnnulus(std::max(0.0, tail - half - 1.0), head + half + 1.0, color, [&](double dx, double dy, double distance) {
            if (distance < 0.0001) return 0u;
            double angle = std::atan2(dy, dx);
            if (angle < 0.0) angle += 2.0 * kPi;
            // Nearest ray direction (rays are offset half a step from the axes).
            constexpr double step = 2.0 * kPi / rays;
            const double nearest = (std::floor(angle / step) + 0.5) * step;
            const double across = std::abs(distance * std::sin(angle - nearest));
            const double along = distance * std::cos(angle - nearest);
            const double lengthCoverage = std::clamp(std::min(along - tail, head - along) + 0.5, 0.0, 1.0);
            const double widthCoverage = std::clamp(half - across + 0.5, 0.0, 1.0);
            return static_cast<unsigned>(maxAlpha * lengthCoverage * widthCoverage + 0.5);
        });
    }

    // A crisp ring with a soft halo on both sides.
    void DrawNeonRing(double radius, double lineWidth, COLORREF color, int opacity) noexcept {
        const double maxAlpha = 255.0 * opacity / 100.0;
        const double half = std::max(0.75, lineWidth * 0.5);
        const double glow = std::max(4.0, lineWidth * 3.0);
        PaintAnnulus(std::max(0.0, radius - half - glow), radius + half + glow, color, [&](double, double, double distance) {
            const double offset = std::abs(distance - radius);
            const double core = std::clamp(half - offset + 0.5, 0.0, 1.0);
            const double haloT = std::clamp(1.0 - std::max(0.0, offset - half) / glow, 0.0, 1.0);
            const double halo = 0.45 * haloT * haloT;
            return static_cast<unsigned>(maxAlpha * std::max(core, halo) + 0.5);
        });
    }

    // A scope closing in on the click: a ring with four inward ticks on the
    // axes. The center stays clear so the pointer tip is never covered.
    void DrawTarget(double radius, double lineWidth, COLORREF color, int opacity) noexcept {
        const double maxAlpha = 255.0 * opacity / 100.0;
        const double half = std::max(0.75, lineWidth * 0.5);
        const double tickStart = radius * 0.55;
        PaintAnnulus(std::max(0.0, tickStart - half - 1.0), radius + half + 1.0, color, [&](double dx, double dy, double distance) {
            const double ring = std::clamp(half - std::abs(distance - radius) + 0.5, 0.0, 1.0);
            const auto tick = [&](double along, double across) {
                return std::clamp(std::min(along - tickStart, radius - along) + 0.5, 0.0, 1.0) *
                       std::clamp(half - across + 0.5, 0.0, 1.0);
            };
            const double ax = std::abs(dx), ay = std::abs(dy);
            return static_cast<unsigned>(maxAlpha * std::max({ring, tick(ax, ay), tick(ay, ax)}) + 0.5);
        });
    }

    // Antialiased outline of a rounded square, for the square magnifier lens.
    void DrawRoundedFrame(double half, double corner, double lineWidth, COLORREF color, int opacity) noexcept {
        const double maxAlpha = 255.0 * opacity / 100.0;
        const double inner = std::max(0.0, half - corner);
        const double radiusLimit = half * 1.4143 + lineWidth;
        PaintAnnulus(0.0, radiusLimit, color, [&](double dx, double dy, double) {
            const double qx = std::abs(dx) - inner, qy = std::abs(dy) - inner;
            const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
            const double distance = outside + std::min(std::max(qx, qy), 0.0) - corner;
            const double coverage = std::clamp(lineWidth * 0.5 - std::abs(distance) + 0.5, 0.0, 1.0);
            return static_cast<unsigned>(maxAlpha * coverage + 0.5);
        });
    }

    HWND CoveredAt(POINT center) const noexcept {
        if (!window || !visible) return nullptr;
        const RECT target{center.x - width / 2, center.y - height / 2,
                          center.x - width / 2 + width, center.y - height / 2 + height};
        return ForeignWindowAbove(window, target);
    }

    bool RepairStack(std::wstring& error) {
        if (!visible || !positioned) return true;
        const HWND obstruction = CoveredAt(lastCenter);
        if (!obstruction) { resistedBy = nullptr; retryStackAfter = 0; return true; }
        const ULONGLONG now = GetTickCount64();
        if (obstruction == resistedBy && now < retryStackAfter) return true;
        if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
            error = Failure(L"Windows could not position a pointer effect", GetLastError());
            return false;
        }
        // Ordinary TOPMOST does not outrank every privileged presentation
        // layer. If the same window remains above us, do not repeatedly fight
        // its order on every mouse movement or accessibility notification.
        resistedBy = CoveredAt(lastCenter);
        retryStackAfter = resistedBy ? now + 500 : 0;
        return true;
    }

    void DrawSparkle(double radius, COLORREF color, int opacity, bool fine = false) noexcept {
        if (!pixels || size <= 0) return;
        const double center = (size - 1) * 0.5;
        const double core = fine ? 0.12 : 0.24;
        const double slope = (1.0 - core) / core;
        const double normal = std::sqrt(1.0 + slope * slope);
        const unsigned red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
        for (int y = 0; y < size; ++y) {
            auto* row = pixels + static_cast<std::size_t>(y) * size;
            const double dy = std::abs(y - center);
            for (int x = 0; x < size; ++x) {
                const double dx = std::abs(x - center);
                const double coverage = std::clamp(
                    (radius - std::max(dx, dy) - slope * std::min(dx, dy)) / normal + 0.5, 0.0, 1.0);
                const unsigned alpha = static_cast<unsigned>(coverage * opacity * 2.55 + 0.5);
                row[x] = Premultiplied(red, green, blue, alpha);
            }
            paintedRows[static_cast<std::size_t>(y)] = {0, size, 0, 0};
        }
    }

    void DrawComet(double radius, double motionX, double motionY, COLORREF color, int opacity) noexcept {
        if (!pixels || size <= 0) return;
        const double length = std::hypot(motionX, motionY);
        const double directionX = length > 0.0 ? motionX / length : 1.0;
        const double directionY = length > 0.0 ? motionY / length : 0.0;
        const double center = (size - 1) * 0.5;
        const double head = radius * 0.5, tail = radius * 2.0;
        const double slope = head / tail;
        const double edgeNormal = std::sqrt(1.0 + slope * slope);
        const unsigned red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
        for (int y = 0; y < size; ++y) {
            auto* row = pixels + static_cast<std::size_t>(y) * size;
            const double dy = y - center;
            for (int x = 0; x < size; ++x) {
                const double dx = x - center;
                const double along = dx * directionX + dy * directionY;
                const double across = std::abs(-dx * directionY + dy * directionX);
                const double distance = along >= 0.0
                    ? head - std::hypot(along, across)
                    : std::min(along + tail, ((along + tail) * slope - across) / edgeNormal);
                const double coverage = std::clamp(distance + 0.5, 0.0, 1.0);
                const unsigned alpha = static_cast<unsigned>(coverage * opacity * 2.55 + 0.5);
                row[x] = Premultiplied(red, green, blue, alpha);
            }
            paintedRows[static_cast<std::size_t>(y)] = {0, size, 0, 0};
        }
    }

    bool Present(POINT center, BYTE fade, std::wstring& error, bool raise = false) {
        return PresentAt({center.x - width / 2, center.y - height / 2}, fade, error, raise);
    }

    // Uploads the pixels at a top-left origin and shows the window.
    bool PresentAt(POINT origin, BYTE fade, std::wstring& error, bool raise = false) {
        if (!Upload(origin, fade, error)) return false;
        if ((!visible || raise) && !SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                           SWP_NOOWNERZORDER | SWP_SHOWWINDOW)) {
            const DWORD code = GetLastError();
            error = Failure(L"Windows could not position a pointer effect", code);
            return false;
        }
        visible = true;
        return true;
    }

    // Uploads pixels and records the placement without changing visibility,
    // so a caller can show several windows in one atomic batch afterwards.
    bool Upload(POINT origin, BYTE fade, std::wstring& error) {
        SIZE dimensions{width, height};
        POINT source{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, fade, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(window, nullptr, &origin, &dimensions, dc,
                                 &source, 0, &blend, ULW_ALPHA)) {
            const DWORD code = GetLastError();
            error = Failure(L"Windows could not display a pointer effect", code);
            return false;
        }
        positioned = true;
        lastOrigin = origin;
        lastCenter = {origin.x + width / 2, origin.y + height / 2};
        lastFade = fade;
        return true;
    }

    // Records a placement performed by an external DeferWindowPos batch.
    void Placed(POINT origin) noexcept {
        visible = true;
        positioned = true;
        lastOrigin = origin;
        lastCenter = {origin.x + width / 2, origin.y + height / 2};
    }

    bool Move(POINT center, std::wstring& error, bool raise = false) {
        if (visible && positioned && lastCenter.x == center.x &&
            lastCenter.y == center.y && !raise) return true;
        const UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
            (visible ? 0u : SWP_SHOWWINDOW) | (raise ? 0u : SWP_NOZORDER);
        if (!SetWindowPos(window, HWND_TOPMOST, center.x - width / 2,
                           center.y - height / 2, 0, 0, flags)) {
            const DWORD code = GetLastError();
            error = Failure(L"Windows could not move the cursor highlight", code);
            return false;
        }
        Placed({center.x - width / 2, center.y - height / 2});
        return true;
    }
};

// The spotlight: one layered window over the pointer's monitor, dark except
// for a soft clear circle. A move rewrites only the rectangle that covers the
// old and the new circle and hands just that rectangle to the compositor, so
// it costs about two circle areas of pixels. Being a single window, the dim
// area and the circle can never be shown out of step with each other.
struct Spotlight {
    HWND window = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ originalBitmap = nullptr;
    std::uint32_t* pixels = nullptr;
    int width = 0, height = 0;
    RECT monitor{};
    std::vector<std::uint32_t> hole; // Premultiplied black, side x side.
    int side = 0, radius = 0;
    std::uint32_t shade = 0;
    RECT holeRect{}; // Last stamped circle in window coordinates; empty when none.
    bool contentReady = false; // The compositor holds the whole surface.
    bool visible = false;
    POINT lastPoint{};
    HWND resistedBy = nullptr;
    ULONGLONG retryStackAfter = 0;

    Spotlight() = default;
    Spotlight(const Spotlight&) = delete;
    Spotlight& operator=(const Spotlight&) = delete;
    ~Spotlight() { Reset(); }

    bool Prepare(HWND controller, bool captureVisible, std::wstring& error) {
        if (window) return true;
        window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kOverlayClass, L"ClickShade spotlight", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
            GetModuleHandleW(nullptr), controller);
        if (!window) { error = Failure(L"Could not create the spotlight window", GetLastError()); return false; }
        ApplyCaptureAffinity(window, captureVisible);
        return true;
    }

    bool Configured(const RECT& monitorRect, int holeRadius, BYTE alpha) const noexcept {
        return bitmap && EqualRect(&monitor, &monitorRect) && radius == holeRadius &&
               shade == (static_cast<std::uint32_t>(alpha) << 24);
    }

    // Sizes the surface to the monitor, rasterizes the circle once and fills
    // everything with the shade. The next Place uploads the whole surface.
    bool Configure(const RECT& monitorRect, int holeRadius, BYTE alpha, std::wstring& error) {
        const int nextWidth = std::max<int>(1, monitorRect.right - monitorRect.left);
        const int nextHeight = std::max<int>(1, monitorRect.bottom - monitorRect.top);
        if (!bitmap || nextWidth != width || nextHeight != height) {
            if (!dc) dc = CreateCompatibleDC(nullptr);
            if (!dc) { error = Failure(L"Could not create the spotlight graphics context", GetLastError()); return false; }
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = nextWidth;
            info.bmiHeader.biHeight = -nextHeight;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* address = nullptr;
            HBITMAP next = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &address, nullptr, 0);
            if (!next || !address) {
                const DWORD code = GetLastError();
                if (next) DeleteObject(next);
                error = Failure(L"Could not allocate the spotlight pixels", code);
                return false;
            }
            HGDIOBJ old = SelectObject(dc, next);
            if (!old || old == HGDI_ERROR) {
                const DWORD code = GetLastError();
                DeleteObject(next);
                error = Failure(L"Could not select the spotlight bitmap", code);
                return false;
            }
            if (!originalBitmap) originalBitmap = old;
            if (bitmap) DeleteObject(bitmap);
            bitmap = next;
            pixels = static_cast<std::uint32_t*>(address);
            width = nextWidth;
            height = nextHeight;
        }
        monitor = monitorRect;
        radius = holeRadius;
        shade = static_cast<std::uint32_t>(alpha) << 24;
        // Odd side: the circle's center lands exactly on the pointer pixel.
        side = holeRadius * 2 + 7;
        hole.assign(static_cast<std::size_t>(side) * side, 0u);
        const double center = (side - 1) * 0.5;
        const double feather = std::max(6.0, holeRadius * 0.22);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const double distance = std::hypot(x - center, y - center);
                const double t = std::clamp((distance - (holeRadius - feather)) / feather, 0.0, 1.0);
                // The rim is exactly the shade, so the stamp has no visible edge.
                const unsigned value = t >= 1.0 ? alpha : static_cast<unsigned>(alpha * t * t * (3.0 - 2.0 * t) + 0.5);
                hole[static_cast<std::size_t>(y) * side + x] = static_cast<std::uint32_t>(value) << 24;
            }
        }
        std::fill(pixels, pixels + static_cast<std::size_t>(width) * height, shade);
        holeRect = {};
        contentReady = false;
        return true;
    }

    void FillShade(const RECT& area) noexcept {
        const LONG left = std::max<LONG>(0, area.left), top = std::max<LONG>(0, area.top);
        const LONG right = std::min<LONG>(width, area.right), bottom = std::min<LONG>(height, area.bottom);
        if (right <= left) return;
        for (LONG y = top; y < bottom; ++y) {
            auto* row = pixels + static_cast<std::size_t>(y) * width;
            std::fill(row + left, row + right, shade);
        }
    }

    void StampHole(const RECT& area) noexcept {
        const LONG left = std::max<LONG>(0, area.left), top = std::max<LONG>(0, area.top);
        const LONG right = std::min<LONG>(width, area.right), bottom = std::min<LONG>(height, area.bottom);
        if (right <= left) return;
        for (LONG y = top; y < bottom; ++y) {
            const auto* source = hole.data() + static_cast<std::size_t>(y - area.top) * side + (left - area.left);
            std::copy(source, source + (right - left), pixels + static_cast<std::size_t>(y) * width + left);
        }
    }

    // Moves the circle to the pointer. Only the union of the old and new
    // circle rectangles is rewritten and uploaded.
    bool Place(POINT point, bool raise, std::wstring& error) {
        const RECT next{point.x - monitor.left - side / 2, point.y - monitor.top - side / 2,
                        point.x - monitor.left - side / 2 + side, point.y - monitor.top - side / 2 + side};
        if (contentReady && visible && !raise && EqualRect(&next, &holeRect)) return true;
        RECT dirty = next;
        if (!IsRectEmpty(&holeRect)) {
            UnionRect(&dirty, &holeRect, &next);
            FillShade(holeRect);
        }
        StampHole(next);
        holeRect = next;
        const RECT surface{0, 0, width, height};
        IntersectRect(&dirty, &dirty, &surface);
        if (!contentReady || !IsRectEmpty(&dirty)) {
            POINT destination{monitor.left, monitor.top};
            SIZE dimensions{width, height};
            POINT source{};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UPDATELAYEREDWINDOWINFO info{};
            info.cbSize = sizeof(info);
            info.pptDst = &destination;
            info.psize = &dimensions;
            info.hdcSrc = dc;
            info.pptSrc = &source;
            info.pblend = &blend;
            info.dwFlags = ULW_ALPHA;
            info.prcDirty = contentReady ? &dirty : nullptr;
            if (!UpdateLayeredWindowIndirect(window, &info)) {
                error = Failure(L"Windows could not display the spotlight", GetLastError());
                return false;
            }
            contentReady = true;
        }
        if ((!visible || raise) && !SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW)) {
            error = Failure(L"Windows could not position the spotlight", GetLastError());
            return false;
        }
        visible = true;
        lastPoint = point;
        return true;
    }

    void Hide() noexcept {
        if (window && visible) ShowWindow(window, SW_HIDE);
        visible = false;
        resistedBy = nullptr;
        retryStackAfter = 0;
    }

    // Raises the dim layer only while another process's window is above it,
    // and leaves a window that keeps winning alone for half a second. Raising
    // unconditionally would wake the stacking listener again with every pass.
    bool RepairStack() noexcept {
        if (!window || !visible) return false;
        const HWND obstruction = ForeignWindowAbove(window, monitor);
        if (!obstruction) { resistedBy = nullptr; retryStackAfter = 0; return false; }
        const ULONGLONG now = GetTickCount64();
        if (obstruction == resistedBy && now < retryStackAfter) return false;
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        resistedBy = ForeignWindowAbove(window, monitor);
        retryStackAfter = resistedBy ? now + 500 : 0;
        return true;
    }

    // Frees the monitor-sized surface; used whenever the spotlight is off.
    void Reset() noexcept {
        if (window) DestroyWindow(window);
        window = nullptr;
        if (dc && originalBitmap) SelectObject(dc, originalBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr;
        bitmap = nullptr;
        originalBitmap = nullptr;
        pixels = nullptr;
        width = height = side = radius = 0;
        hole.clear();
        hole.shrink_to_fit();
        holeRect = {};
        monitor = {};
        contentReady = visible = false;
    }
};

// The magnifier lens: a color-keyed host, the Magnification API child clipped
// to a circle or rounded square, and an antialiased frame surface on top.
struct Lens {
    HWND host = nullptr;
    HWND magnifier = nullptr;
    Surface frame;
    int size = 0, shape = -1, border = 0;
    int inner = 0; // Side of the magnifier child, inside the frame.
    COLORREF borderColor = 0;
    bool frameDrawn = true; // False: the frame surface stays fully transparent.
    double zoom = 0.0; // Transform applied to the child; 0 until set.
    bool cursorShown = false;
    bool visible = false;
    bool framePresented = false;
    POINT lastCenter{};
    RECT lastSource{};
    std::vector<HWND> filtered;

    Lens() = default;
    Lens(const Lens&) = delete;
    Lens& operator=(const Lens&) = delete;
    ~Lens() { Reset(); }

    bool Ready() const noexcept { return host && magnifier && frame.window; }

    // Creating the Magnification API child costs well over 100 ms, so the
    // windows are created once and later changes (size, shape, frame color,
    // cursor drawing) adjust them in place.
    bool Prepare(HWND controller, int sizePx, int shapeKind, int borderPx, COLORREF color, bool drawFrame,
                 bool showCursor, bool captureVisible, std::wstring& error) {
        if (Ready() && sizePx == size && shapeKind == shape && borderPx == border &&
            color == borderColor && drawFrame == frameDrawn && showCursor == cursorShown) return true;
        const int inset = std::max(1, borderPx / 2);
        const int nextInner = std::max(4, sizePx - 2 * inset);
        if (!host) {
            host = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                kLensClass, L"ClickShade magnifier", WS_POPUP | WS_CLIPCHILDREN, 0, 0, sizePx, sizePx,
                nullptr, nullptr, GetModuleHandleW(nullptr), controller);
            if (!host) { error = Failure(L"Could not create the magnifier window", GetLastError()); return false; }
            SetLayeredWindowAttributes(host, kLensKey, 255, LWA_COLORKEY | LWA_ALPHA);
            ApplyCaptureAffinity(host, captureVisible);
        } else if (sizePx != size) {
            SetWindowPos(host, nullptr, 0, 0, sizePx, sizePx, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        if (!magnifier) {
            magnifier = CreateWindowExW(0, L"Magnifier", L"ClickShade lens",
                WS_CHILD | WS_VISIBLE | (showCursor ? kMagnifierShowCursor : 0), inset, inset, nextInner, nextInner,
                host, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!magnifier) {
                error = Failure(L"Could not create the magnifier lens", GetLastError());
                Reset();
                return false;
            }
            zoom = 0.0;
        } else {
            SetWindowPos(magnifier, nullptr, inset, inset, nextInner, nextInner, SWP_NOZORDER | SWP_NOACTIVATE);
            const LONG_PTR style = GetWindowLongPtrW(magnifier, GWL_STYLE);
            const LONG_PTR wanted = showCursor ? (style | kMagnifierShowCursor) : (style & ~static_cast<LONG_PTR>(kMagnifierShowCursor));
            if (wanted != style) SetWindowLongPtrW(magnifier, GWL_STYLE, wanted);
        }
        inner = nextInner;
        const int corner = std::max(8, sizePx / 5);
        HRGN region = shapeKind == 0 ? CreateEllipticRgn(0, 0, inner + 1, inner + 1)
                                     : CreateRoundRectRgn(0, 0, inner + 1, inner + 1, corner, corner);
        if (region) SetWindowRgn(magnifier, region, TRUE); // The system owns the region now.
        if (!frame.PrepareRect(sizePx, sizePx, controller, captureVisible, error, L"ClickShade magnifier frame")) {
            Reset();
            return false;
        }
        // The frame straddles the child's clipped edge: one pixel outside it
        // and the border width inside, hiding the region's aliased rim. With
        // the frame switched off the surface stays fully transparent.
        const double line = borderPx + 2.0;
        const double edge = sizePx * 0.5 - inset + 1.0;
        if (!drawFrame) frame.Clear();
        else if (shapeKind == 0) frame.DrawCircle(edge, line, false, color, 100);
        else frame.DrawRoundedFrame(edge - line * 0.5, std::max(1.0, corner * 0.5), line, color, 100);
        const bool resized = sizePx != size;
        size = sizePx; shape = shapeKind; border = borderPx; borderColor = color; frameDrawn = drawFrame; cursorShown = showCursor;
        framePresented = false;
        // A new size moves the window's top-left even for an unchanged center.
        if (resized) lastCenter = {LONG_MIN, LONG_MIN};
        return true;
    }

    // The child enlarges its source rectangle by this factor. Without it the
    // Magnification API draws the source at its original size.
    bool SetZoom(double factor, const MagnificationApi& api) {
        if (factor == zoom) return true;
        MagnifierTransform transform{};
        transform.v[0][0] = static_cast<float>(factor);
        transform.v[1][1] = static_cast<float>(factor);
        transform.v[2][2] = 1.0f;
        if (!api.setTransform(magnifier, &transform)) return false;
        if (api.setSmoothing) api.setSmoothing(magnifier, TRUE);
        zoom = factor;
        return true;
    }

    // Keeps the magnified content free of ClickShade's own overlays. The
    // candidates come in priority order; Windows rejects lists longer than
    // kMagnifierFilterLimit, so the least important ones are left out.
    bool Filter(const std::vector<HWND>& candidates, const MagnificationApi& api) {
        std::vector<HWND> windows{host, frame.window};
        for (HWND window : candidates) {
            if (windows.size() >= kMagnifierFilterLimit) break;
            if (window && std::find(windows.begin(), windows.end(), window) == windows.end()) windows.push_back(window);
        }
        std::vector<HWND> sorted = windows;
        std::sort(sorted.begin(), sorted.end());
        if (sorted == filtered) return true;
        if (!api.setFilter(magnifier, kMagnifierFilterExclude, static_cast<int>(windows.size()), windows.data())) return false;
        filtered = std::move(sorted);
        return true;
    }

    bool Show(POINT center, const RECT& source, bool raise, const MagnificationApi& api, std::wstring& error) {
        const POINT origin{center.x - size / 2, center.y - size / 2};
        if (!framePresented) {
            if (!frame.Upload(origin, 255, error)) return false;
            framePresented = true;
        }
        const bool moved = !visible || lastCenter.x != center.x || lastCenter.y != center.y;
        const bool reorder = !visible || raise;
        if (moved || reorder) {
            // Host first, then the frame, so the frame ends above the host.
            const UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW | (reorder ? 0u : SWP_NOZORDER);
            HDWP defer = BeginDeferWindowPos(2);
            if (defer) defer = DeferWindowPos(defer, host, HWND_TOPMOST, origin.x, origin.y, 0, 0, flags);
            if (defer) defer = DeferWindowPos(defer, frame.window, HWND_TOPMOST, origin.x, origin.y, 0, 0, flags);
            if (!defer || !EndDeferWindowPos(defer)) {
                error = Failure(L"Windows could not position the magnifier", GetLastError());
                return false;
            }
            frame.Placed(origin);
        }
        // Every call re-reads the screen: unchanged geometry still needs a
        // refresh because the content under the lens changes on its own.
        if (!api.setSource(magnifier, source)) {
            error = Failure(L"Windows could not refresh the magnifier", GetLastError());
            return false;
        }
        visible = true;
        lastCenter = center;
        lastSource = source;
        return true;
    }

    void Hide() noexcept {
        if (visible) {
            if (host) ShowWindow(host, SW_HIDE);
            frame.Hide();
        }
        visible = false;
    }

    bool Covered() const noexcept { return visible && frame.CoveredAt(frame.lastCenter) != nullptr; }

    void Raise() noexcept {
        if (!visible) return;
        SetWindowPos(host, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SetWindowPos(frame.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    void Reset() noexcept {
        if (magnifier) DestroyWindow(magnifier);
        if (host) DestroyWindow(host);
        magnifier = host = nullptr;
        frame.Reset();
        visible = framePresented = false;
        size = inner = 0; shape = -1;
        zoom = 0.0;
        filtered.clear();
    }
};

// One row of the key display: the keycaps of a shortcut, or the characters
// typed in a row (every-key mode), with a repeat count.
struct KeyStroke {
    std::vector<std::wstring> caps;
    std::wstring typed;
    unsigned repeat = 1;
    LONGLONG updated = 0; // Last change; the hold time starts here.
    // The row at full opacity, premultiplied, shadow margin included.
    std::vector<std::uint32_t> pixels;
    int width = 0, height = 0;
    bool dirty = true;
};

// Sizes of the key display at the current scale.
struct KeyMetrics {
    int capHeight = 0, capPadding = 0, capGap = 0, capFont = 0, typeFont = 0, countFont = 0;
    int padX = 0, padY = 0, margin = 0, shadow = 0, shadowOffset = 0, rowGap = 0, depth = 0;
    double corner = 0.0, capCorner = 0.0;
    bool operator==(const KeyMetrics& other) const noexcept {
        return capHeight == other.capHeight && capFont == other.capFont && typeFont == other.typeFont;
    }
};

inline void BlendOver(std::uint32_t& target, unsigned red, unsigned green, unsigned blue, double alpha) noexcept {
    if (alpha <= 0.0) return;
    const unsigned a = static_cast<unsigned>(std::clamp(alpha * 255.0 + 0.5, 0.0, 255.0));
    const unsigned keep = 255 - a;
    const auto mix = [&](unsigned shift, unsigned value) {
        return std::min(255u, (value * a + 127) / 255 + (((target >> shift) & 255u) * keep + 127) / 255);
    };
    target = (std::min(255u, a + (((target >> 24) & 255u) * keep + 127) / 255) << 24) |
             (mix(16, red) << 16) | (mix(8, green) << 8) | mix(0, blue);
}

// Signed distance from a pixel center to a rounded rectangle (negative inside).
inline double RoundedDistance(double x, double y, double left, double top, double width, double height, double radius) noexcept {
    const double qx = std::abs(x - (left + width * 0.5)) - (width * 0.5 - radius);
    const double qy = std::abs(y - (top + height * 0.5)) - (height * 0.5 - radius);
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - radius;
}

// The key display, in the style of the Mac keystroke visualizers: each
// shortcut is a row of keycaps on a rounded, translucent panel with a soft
// shadow; typed characters join into one row. Rows stack towards the screen
// edge and fade out one by one. Everything is one layered window.
struct KeyPanel {
    Surface surface;
    HDC textDc = nullptr;
    HBITMAP textBitmap = nullptr;
    HGDIOBJ textOriginal = nullptr;
    std::uint32_t* textPixels = nullptr;
    int textWidth = 0, textHeight = 0;
    HFONT capFont = nullptr, typeFont = nullptr, countFont = nullptr;
    KeyMetrics metrics;
    std::vector<KeyStroke> strokes; // Oldest first.
    POINT origin{};
    bool visible = false;

    KeyPanel() = default;
    KeyPanel(const KeyPanel&) = delete;
    KeyPanel& operator=(const KeyPanel&) = delete;
    ~KeyPanel() { Reset(); }

    void Hide() noexcept {
        surface.Hide();
        visible = false;
        strokes.clear();
    }

    void ReleaseFonts() noexcept {
        for (HFONT* font : {&capFont, &typeFont, &countFont}) {
            if (*font) DeleteObject(*font);
            *font = nullptr;
        }
        metrics = {};
    }

    void Reset() noexcept {
        surface.Reset();
        if (textDc && textOriginal) SelectObject(textDc, textOriginal);
        if (textBitmap) DeleteObject(textBitmap);
        if (textDc) DeleteDC(textDc);
        textDc = nullptr; textBitmap = nullptr; textOriginal = nullptr; textPixels = nullptr;
        textWidth = textHeight = 0;
        ReleaseFonts();
        strokes.clear();
        visible = false;
    }

    static KeyMetrics MetricsFor(unsigned dpi, int sizePercent) noexcept {
        const double s = dpi / 96.0 * sizePercent / 100.0;
        const auto px = [&](double value, int minimum) { return std::max(minimum, static_cast<int>(std::lround(value * s))); };
        KeyMetrics m;
        m.capHeight = px(44.0, 16);
        m.capPadding = px(13.0, 4);
        m.capGap = px(7.0, 2);
        m.capFont = px(22.0, 8);
        m.typeFont = px(28.0, 10);
        m.countFont = px(19.0, 7);
        m.padX = px(11.0, 4);
        m.padY = px(11.0, 3);
        m.shadow = px(14.0, 3);
        m.shadowOffset = px(3.0, 1);
        m.margin = m.shadow + m.shadowOffset;
        m.rowGap = px(8.0, 2);
        m.depth = px(2.0, 1);
        m.corner = std::max(4.0, 16.0 * s);
        m.capCorner = std::max(3.0, 8.0 * s);
        return m;
    }

    bool EnsureFonts(const KeyMetrics& next, std::wstring& error) {
        if (capFont && next == metrics) return true;
        ReleaseFonts();
        const auto make = [](int pixels, int weight) {
            return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        };
        capFont = make(next.capFont, FW_SEMIBOLD);
        typeFont = make(next.typeFont, FW_SEMIBOLD);
        countFont = make(next.countFont, FW_SEMIBOLD);
        if (!capFont || !typeFont || !countFont) {
            error = Failure(L"Could not create the key display font", GetLastError());
            ReleaseFonts();
            return false;
        }
        metrics = next;
        for (auto& stroke : strokes) stroke.dirty = true;
        return true;
    }

    bool EnsureText(int width, int height, std::wstring& error) {
        if (textDc && width <= textWidth && height <= textHeight) return true;
        if (!textDc) textDc = CreateCompatibleDC(nullptr);
        if (!textDc) { error = Failure(L"Could not create the key display text context", GetLastError()); return false; }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = std::max(width, textWidth);
        info.bmiHeader.biHeight = -std::max(height, textHeight);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* address = nullptr;
        HBITMAP next = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &address, nullptr, 0);
        if (!next || !address) {
            if (next) DeleteObject(next);
            error = Failure(L"Could not allocate key display text pixels", GetLastError());
            return false;
        }
        HGDIOBJ old = SelectObject(textDc, next);
        if (!textOriginal) textOriginal = old;
        if (textBitmap) DeleteObject(textBitmap);
        textBitmap = next;
        textPixels = static_cast<std::uint32_t*>(address);
        textWidth = info.bmiHeader.biWidth;
        textHeight = -info.bmiHeader.biHeight;
        return true;
    }

    SIZE Measure(HFONT font, const std::wstring& text) {
        SIZE extent{};
        HGDIOBJ previous = SelectObject(textDc, font);
        GetTextExtentPoint32W(textDc, text.c_str(), static_cast<int>(text.size()), &extent);
        SelectObject(textDc, previous);
        return extent;
    }

    // Renders one row at full opacity into its own pixel buffer.
    bool RenderStroke(KeyStroke& stroke, const Config& config, int maximumWidth, std::wstring& error) {
        const KeyMetrics& m = metrics;
        const std::wstring count = stroke.repeat > 1 ? L"×" + std::to_wstring(stroke.repeat) : std::wstring();
        const int countWidth = count.empty() ? 0 : Measure(countFont, count).cx + m.capGap * 2;
        // Content: keycaps, or typed text trimmed from the front to fit.
        std::vector<int> capWidths;
        std::wstring typed = stroke.typed;
        int contentWidth = 0;
        const int textInset = m.padX + m.capPadding / 2;
        if (!stroke.caps.empty()) {
            for (const auto& cap : stroke.caps) {
                capWidths.push_back(std::max<int>(m.capHeight, Measure(capFont, cap).cx + 2 * m.capPadding));
                contentWidth += capWidths.back() + (capWidths.size() > 1 ? m.capGap : 0);
            }
        } else {
            const int available = std::max(m.capHeight, maximumWidth - 2 * m.margin - 2 * textInset - countWidth);
            int width = Measure(typeFont, typed).cx;
            if (width > available) {
                // Keep the newest characters; an ellipsis marks the cut.
                while (typed.size() > 1 && Measure(typeFont, L"…" + typed).cx > available) typed.erase(0, 1);
                typed.insert(0, L"…");
                width = Measure(typeFont, typed).cx;
            }
            contentWidth = width + 2 * (textInset - m.padX);
        }
        const int bubbleWidth = contentWidth + 2 * m.padX + countWidth;
        const int bubbleHeight = m.capHeight + 2 * m.padY;
        const int width = bubbleWidth + 2 * m.margin, height = bubbleHeight + 2 * m.margin;
        if (!EnsureText(width, height, error)) return false;
        stroke.pixels.assign(static_cast<std::size_t>(width) * height, 0u);
        stroke.width = width;
        stroke.height = height;

        const unsigned red = GetRValue(config.keyColor), green = GetGValue(config.keyColor), blue = GetBValue(config.keyColor);
        const bool dark = red * 299 + green * 587 + blue * 114 < 140000;
        const double fill = config.keyOpacity / 100.0;
        const unsigned ink = dark ? 246u : 24u;
        const double bubbleLeft = m.margin, bubbleTop = m.margin;

        // Glyph coverage: white text on black in the scratch DIB.
        std::memset(textPixels, 0, static_cast<std::size_t>(textWidth) * textHeight * 4);
        SetBkMode(textDc, TRANSPARENT);
        SetTextColor(textDc, RGB(255, 255, 255));
        const auto draw = [&](HFONT font, const std::wstring& text, RECT rect) {
            HGDIOBJ previous = SelectObject(textDc, font);
            DrawTextW(textDc, text.c_str(), static_cast<int>(text.size()), &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
            SelectObject(textDc, previous);
        };
        struct CapBox { double left, top, width, height; };
        std::vector<CapBox> boxes;
        const int capTop = m.margin + m.padY;
        if (!stroke.caps.empty()) {
            int x = m.margin + m.padX;
            for (std::size_t n = 0; n < stroke.caps.size(); ++n) {
                boxes.push_back({static_cast<double>(x), static_cast<double>(capTop),
                                 static_cast<double>(capWidths[n]), static_cast<double>(m.capHeight)});
                // The label sits on the key's face, above its darker lower edge.
                draw(capFont, stroke.caps[n], {x, capTop, x + capWidths[n], capTop + m.capHeight - m.depth});
                x += capWidths[n] + m.capGap;
            }
        } else {
            const int x = m.margin + textInset;
            draw(typeFont, typed, {x, capTop, x + contentWidth - 2 * (textInset - m.padX), capTop + m.capHeight});
        }
        if (!count.empty()) {
            const int right = m.margin + bubbleWidth - m.padX;
            draw(countFont, count, {right - countWidth + m.capGap, capTop, right, capTop + m.capHeight});
        }
        GdiFlush();

        for (int y = 0; y < height; ++y) {
            auto* row = stroke.pixels.data() + static_cast<std::size_t>(y) * width;
            const auto* glyphs = textPixels + static_cast<std::size_t>(y) * textWidth;
            const double cy = y + 0.5;
            for (int x = 0; x < width; ++x) {
                const double cx = x + 0.5;
                std::uint32_t pixel = 0;
                const double d = RoundedDistance(cx, cy, bubbleLeft, bubbleTop, bubbleWidth, bubbleHeight, m.corner);
                const double inside = std::clamp(0.5 - d, 0.0, 1.0);
                // Soft shadow below the panel, only where the panel is not.
                const double ds = RoundedDistance(cx, cy - m.shadowOffset, bubbleLeft, bubbleTop, bubbleWidth, bubbleHeight, m.corner);
                if (ds < m.shadow && inside < 1.0) {
                    const double t = std::clamp(1.0 - std::max(ds, 0.0) / m.shadow, 0.0, 1.0);
                    BlendOver(pixel, 0, 0, 0, 0.30 * t * t * (1.0 - inside));
                }
                if (inside > 0.0) {
                    BlendOver(pixel, red, green, blue, fill * inside);
                    // Hairline rim, like a macOS HUD.
                    const double rim = std::clamp(1.0 - std::abs(d + 0.5), 0.0, 1.0);
                    BlendOver(pixel, dark ? 255 : 0, dark ? 255 : 0, dark ? 255 : 0, (dark ? 0.14 : 0.10) * rim);
                    for (const auto& box : boxes) {
                        if (cx < box.left - 1 || cx > box.left + box.width + 1 || cy < box.top - 1 || cy > box.top + box.height + 1) continue;
                        // Key base (its lower edge shows as depth), then the face.
                        const double base = std::clamp(0.5 - RoundedDistance(cx, cy, box.left, box.top, box.width, box.height, m.capCorner), 0.0, 1.0);
                        const double face = std::clamp(0.5 - RoundedDistance(cx, cy, box.left, box.top, box.width, box.height - m.depth, m.capCorner), 0.0, 1.0);
                        if (dark) {
                            BlendOver(pixel, 255, 255, 255, 0.07 * base);
                            BlendOver(pixel, 255, 255, 255, 0.13 * face);
                        } else {
                            BlendOver(pixel, 0, 0, 0, 0.16 * base);
                            BlendOver(pixel, 255, 255, 255, 0.85 * face);
                        }
                    }
                    const unsigned coverage = (glyphs[x] >> 16) & 255u;
                    if (coverage) {
                        const bool counter = !count.empty() && x >= m.margin + bubbleWidth - m.padX - countWidth + m.capGap;
                        BlendOver(pixel, ink, ink + 2u, ink + (dark ? 6u : 8u), coverage / 255.0 * (counter ? 0.62 : 1.0));
                    }
                }
                row[x] = pixel;
            }
        }
        stroke.dirty = false;
        return true;
    }

    // Composes every row into the window surface with its own fade. Returns
    // the surface size through the out parameters; false on a GDI failure.
    bool Compose(const Config& config, int maximumWidth, const std::vector<double>& fades, HWND controller, std::wstring& error) {
        for (auto& stroke : strokes)
            if (stroke.dirty && !RenderStroke(stroke, config, maximumWidth, error)) return false;
        const KeyMetrics& m = metrics;
        int width = 0;
        for (const auto& stroke : strokes) width = std::max(width, stroke.width);
        const int pitch = m.capHeight + 2 * m.padY + m.rowGap;
        const int rows = static_cast<int>(strokes.size());
        const int height = 2 * m.margin + rows * (m.capHeight + 2 * m.padY) + (rows - 1) * m.rowGap;
        if (!surface.PrepareRect(width, height, controller, config.captureVisible, error, L"ClickShade key display")) return false;
        surface.Clear();
        const bool top = config.keyPosition == 1;
        for (int n = 0; n < rows; ++n) {
            const auto& stroke = strokes[static_cast<std::size_t>(n)];
            // The newest row sits nearest the screen edge.
            const int slot = top ? rows - 1 - n : n;
            const int y0 = slot * pitch;
            const int x0 = config.keyPosition == 2 ? 0 : config.keyPosition == 3 ? width - stroke.width : (width - stroke.width) / 2;
            const unsigned fade = static_cast<unsigned>(std::clamp(fades[static_cast<std::size_t>(n)] * 255.0 + 0.5, 0.0, 255.0));
            if (!fade) continue;
            for (int y = 0; y < stroke.height; ++y) {
                const int ty = y0 + y;
                if (ty < 0 || ty >= height) continue;
                auto* target = surface.pixels + static_cast<std::size_t>(ty) * width + x0;
                const auto* source = stroke.pixels.data() + static_cast<std::size_t>(y) * stroke.width;
                for (int x = 0; x < stroke.width; ++x) {
                    std::uint32_t value = source[x];
                    if (!value) continue;
                    if (fade < 255) {
                        const auto scale = [&](unsigned shift) { return ((((value >> shift) & 255u) * fade + 127) / 255) << shift; };
                        value = scale(24) | scale(16) | scale(8) | scale(0);
                    }
                    // Premultiplied "over": shadows of neighbouring rows overlap.
                    const unsigned keep = 255 - (value >> 24);
                    const std::uint32_t under = target[x];
                    const auto add = [&](unsigned shift) {
                        return std::min(255u, ((value >> shift) & 255u) + (((under >> shift) & 255u) * keep + 127) / 255) << shift;
                    };
                    target[x] = add(24) | add(16) | add(8) | add(0);
                }
            }
        }
        surface.MarkAllPainted();
        return true;
    }

    bool Present(POINT topLeft, std::wstring& error, bool raise = false) {
        if (!surface.PresentAt(topLeft, 255, error, raise)) return false;
        origin = topLeft;
        visible = true;
        return true;
    }
};

struct Particle {
    Surface surface;
    bool active = false;
    POINT origin{};
    LONGLONG started = 0;
    unsigned dpi = 96;
    int radius = 42;
    int duration = 360;
    int opacity = 80;
    COLORREF color = 0;
    int style = kClickRing;
    int thickness = 3;
    int easing = 1;
};

struct TrailStamp {
    Surface surface;
    bool active = false;
    POINT origin{};
    LONGLONG started = 0;
};

struct QueuedClick {
    // 0=free, 1=producer writing, 2=ready. No allocations or locks on clicks.
    std::atomic<unsigned> state{0};
    unsigned buttons = 0;
    POINT origin{};
    LONGLONG started = 0;
};

int ParticlePadding(int style, int thicknessPixels) noexcept {
    return style == kClickNeon ? std::max(4, thicknessPixels * 3) + 2 : 0;
}

}  // namespace

struct VisualEffects::Impl {
    std::thread worker;
    std::atomic<HWND> controller{nullptr};
    std::atomic<DWORD> threadId{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> accepting{false};
    std::atomic<bool> renderHealthy{true};
    std::atomic<bool> allowed{false};
    std::atomic<bool> keyAllowed{false}; // Rules for the key display: it survives a hidden pointer.
    std::atomic<bool> pointerRequested{false};
    std::atomic<bool> keyRequested{false};
    std::atomic<unsigned> clickButtons{0};
    std::atomic<bool> pointerDirty{false};
    std::atomic<bool> pointerScheduled{false};
    std::atomic<bool> settingsQueued{false};
    std::atomic<bool> allowedQueued{false};
    std::atomic<bool> stackWatching{false};
    std::atomic<bool> stackQueued{false};
    std::atomic<HWND> notifyWindow{nullptr};
    std::atomic<UINT> notifyMessage{0};
    std::array<QueuedClick, kClickQueueSize> clicks;
    std::atomic<unsigned> clickSequence{0};
    std::atomic<std::uint64_t> frameCount{0};
    std::atomic<std::uint64_t> highlightMoves{0};
    std::atomic<std::uint64_t> highlightRasterizations{0};
    std::atomic<std::uint64_t> clickFrames{0};
    std::atomic<std::uint64_t> pointerWakeups{0};
    std::atomic<std::uint64_t> stackWakeups{0};
    std::atomic<std::uint64_t> magnifierUpdates{0};
    std::atomic<std::uint64_t> keyDisplayUpdates{0};
    std::atomic<bool> lensVisible{false};
    std::atomic<bool> keyVisible{false};
    std::atomic<unsigned> effectiveRefreshRate{60};
    std::atomic<unsigned> effectiveMotionRate{60};
    std::atomic<unsigned> effectiveDpi{96};
    std::atomic<bool> highResolutionTimer{false};
    std::atomic<bool> framePending{false};

    std::mutex configMutex;
    Config pending;
    mutable std::mutex errorMutex;
    std::wstring errorText;
    std::mutex startMutex;
    std::condition_variable startCondition;
    bool startComplete = false;
    bool startSuccess = false;
    std::wstring startError;

    // Renderer thread only from here down.
    Config config;
    Surface highlight;
    Spotlight spotlight;
    std::array<Particle, kParticleCount> particles;
    std::array<TrailStamp, kTrailCount> trail;
    std::size_t nextTrail = 0;
    bool trailPointReady = false;
    POINT lastTrailPoint{};
    LONGLONG lastTrailStamp = 0;
    HANDLE frameTimer = nullptr;
    bool frameArmed = false;
    LONGLONG frequency = 0;
    LONGLONG frameInterval = 1;
    LONGLONG motionInterval = 1;
    LONGLONG nextFrame = 0;
    LONGLONG nextMotionFrame = 0;
    LONGLONG lastAnimationFrame = 0;
    LONGLONG nextAnimationFrame = 0;
    LONGLONG nextStackCheck = 0;
    LONGLONG armedDeadline = 0;
    LONGLONG lastPointerActivity = 0;
    LONGLONG nextBreathFrame = 0;
    LONGLONG breathOrigin = 0;
    bool idleHidden = false;
    AdaptiveFramePacer pacer;
    bool trailMotionPending = false;
    bool highlightPixelsReady = false;
    bool faulted = false;
    bool captureAffinityReported = false;
    HMONITOR currentMonitor = nullptr;
    RECT currentMonitorRect{};
    unsigned monitorDpi = 96;
    HMODULE shcore = nullptr;
    WinEventListener stackListener;
    bool stackDirty = false;
    using DpiFunction = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    DpiFunction getDpiForMonitor = nullptr;
    MagnificationApi magnification;
    Lens lens;
    bool lensUnavailable = false; // Set after a Magnification API failure until settings change.
    LONGLONG nextLensFrame = 0;
    POINT lensPointer{}; // Pointer position the lens was last placed for.
    KeyPanel keyPanel;
    LONGLONG nextKeyFrame = 0;

    static void StackEvent(void* context, DWORD event, HWND window,
                           LONG object, LONG child, DWORD) noexcept {
        auto* self = static_cast<Impl*>(context);
        const bool menu = event >= EVENT_SYSTEM_MENUSTART && event <= EVENT_SYSTEM_MENUPOPUPEND;
        const bool reorder = event == EVENT_OBJECT_REORDER;
        if (!self->accepting.load() || !self->allowed.load() || !self->renderHealthy.load() ||
            !self->stackWatching.load() || self->stackQueued.load() ||
            (!menu && !reorder && child != CHILDID_SELF))
            return;
        if (!menu && object != OBJID_WINDOW && object != OBJID_MENU &&
            !(event == EVENT_OBJECT_REORDER && window == GetDesktopWindow() && object == OBJID_CLIENT))
            return;
        if (!menu && window && window != GetDesktopWindow()) {
            if (GetAncestor(window, GA_ROOT) != window) return;
            // Moving ordinary windows cannot cover a topmost effect. Ignore
            // those events instead of waking for every application drag.
            if (event == EVENT_OBJECT_LOCATIONCHANGE &&
                !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST)) return;
        }
        if (self->stackQueued.exchange(true)) return;
        const HWND target = self->controller.load();
        if (!target || !PostMessageW(target, kStackChanged, 0, 0)) self->stackQueued.store(false);
    }

    void Notify() noexcept {
        HWND target = notifyWindow.load();
        const UINT message = notifyMessage.load();
        if (target && message) PostMessageW(target, message, 0, 0);
    }

    void SetError(const std::wstring& text) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            changed = errorText != text;
            errorText = text;
        }
        if (changed) Notify();
    }

    void PublishRequested(const Config& value) noexcept {
        pointerRequested.store(value.enabled && ((value.highlight && (value.highlightOpacity > 0 || value.highlightStyle == kHighlightSpotlight)) ||
                                                (value.trail && value.trailOpacity > 0) || value.magnifier));
        clickButtons.store(value.enabled && value.clicks && value.clickOpacity > 0 ? value.buttons : 0);
        keyRequested.store(value.enabled && value.keyDisplay);
    }

    unsigned DpiAt(POINT point) noexcept {
        UINT x = 96, y = 96;
        if (getDpiForMonitor) {
            HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
            if (FAILED(getDpiForMonitor(monitor, 0, &x, &y))) x = 96;
        } else {
            const UINT system = GetDpiForSystem();
            if (system) x = system;
        }
        return std::clamp(x, 96u, 480u);
    }

    static int PixelRadius(int dip, unsigned dpi) noexcept {
        return std::clamp(MulDiv(dip, static_cast<int>(dpi), 96), 2, 1600);
    }

    bool MayShow() const noexcept {
        return accepting.load() && allowed.load() && config.enabled && !faulted;
    }

    // The key display follows the same rules except the hidden-pointer pause:
    // many editors hide the pointer while typing, which is exactly when a
    // presenter wants the keys shown.
    bool MayShowKeys() const noexcept {
        return accepting.load() && keyAllowed.load() && config.enabled && !faulted;
    }

    bool HighlightWanted() const noexcept {
        return config.highlight && (config.highlightOpacity > 0 || config.highlightStyle == kHighlightSpotlight);
    }

    bool LensWanted() const noexcept { return config.magnifier && !lensUnavailable; }

    bool PointerWanted() const noexcept {
        return HighlightWanted() || (config.trail && config.trailOpacity > 0) || LensWanted();
    }

    static unsigned RefreshRateAt(HMONITOR monitor) noexcept {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)) &&
            EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) &&
            (mode.dmFields & DM_DISPLAYFREQUENCY) &&
            mode.dmDisplayFrequency > 1)
            return std::clamp(static_cast<unsigned>(mode.dmDisplayFrequency), 30u, 360u);
        return 60;
    }

    void UpdateMonitor(POINT point, bool force = false) noexcept {
        const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
        if (!force && monitor == currentMonitor) return;
        currentMonitor = monitor;
        MONITORINFO info{}; info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) currentMonitorRect = info.rcMonitor;
        else currentMonitorRect = {point.x - 1, point.y - 1, point.x + 1, point.y + 1};
        const unsigned dpi = DpiAt(point);
        if (dpi != monitorDpi) highlightPixelsReady = false;
        monitorDpi = dpi;
        const unsigned previousDpi = effectiveDpi.exchange(dpi, std::memory_order_relaxed);
        const unsigned monitorRate = RefreshRateAt(monitor);
        const bool changed = pacer.Configure(monitorRate, config.refreshRate);
        const unsigned rate = pacer.Rate();
        const unsigned motionRate = config.refreshRate ? config.refreshRate : pacer.MonitorCap();
        const unsigned previousRate = effectiveRefreshRate.exchange(rate, std::memory_order_relaxed);
        frameInterval = std::max<LONGLONG>(1, (frequency + rate - 1) / rate);
        motionInterval = std::max<LONGLONG>(1, (frequency + motionRate - 1) / motionRate);
        effectiveMotionRate.store(motionRate, std::memory_order_relaxed);
        if (changed) {
            nextMotionFrame = nextAnimationFrame = lastAnimationFrame = 0;
        }
        if (previousDpi != dpi || previousRate != rate) Notify();
    }

    bool AnyParticles() const noexcept {
        for (const auto& particle : particles) if (particle.active) return true;
        return false;
    }

    bool AnyTrail() const noexcept {
        for (const auto& stamp : trail) if (stamp.active) return true;
        return false;
    }

    void ClearTrail() noexcept {
        for (auto& stamp : trail) { stamp.active = false; stamp.surface.Hide(); }
        nextTrail = 0;
        trailPointReady = false;
        lastTrailStamp = 0;
        trailMotionPending = false;
    }

    void CancelFrame() noexcept {
        if (frameArmed && frameTimer) CancelWaitableTimer(frameTimer);
        frameArmed = false;
        framePending.store(false, std::memory_order_relaxed);
    }

    void ClearParticles() noexcept {
        for (auto& particle : particles) {
            particle.active = false;
            particle.surface.Hide();
        }
    }

    void HideHighlight() noexcept {
        highlight.Hide();
        spotlight.Hide();
    }

    bool HighlightVisible() const noexcept { return highlight.visible || spotlight.visible; }

    bool SpotlightWanted() const noexcept {
        return config.highlight && config.highlightStyle == kHighlightSpotlight;
    }

    void HideLens() noexcept {
        lens.Hide();
        lensVisible.store(false, std::memory_order_relaxed);
        nextLensFrame = 0;
    }

    void HideKeys() noexcept {
        keyPanel.Hide();
        keyVisible.store(false, std::memory_order_relaxed);
        nextKeyFrame = 0;
    }

    // Everything that follows the pointer. The key display stays.
    void HidePointer() noexcept {
        HideHighlight();
        HideLens();
        pointerDirty.store(false);
        pointerScheduled.store(false);
        ClearParticles();
        ClearTrail();
        idleHidden = false;
        nextMotionFrame = lastAnimationFrame = nextAnimationFrame = nextBreathFrame = 0;
        pacer.Observe(0.0, 0.0, false);
    }

    // The pointer is hidden or suppressed while the effects are otherwise
    // allowed. Editors hide it while you type, so the spotlight and the lens
    // stay where they are: a dimmed screen that flashes on and off with every
    // keystroke is worse than a spotlight that waits. What is drawn at the
    // pointer itself (halo, trail, click rings) hides with it.
    void PointerHidden() noexcept {
        if (!MayShowKeys()) { HidePointer(); return; }
        highlight.Hide();
        if (!SpotlightWanted() || idleHidden) spotlight.Hide();
        if (!LensWanted()) HideLens();
        pointerDirty.store(false);
        pointerScheduled.store(false);
        ClearParticles();
        ClearTrail();
        nextMotionFrame = lastAnimationFrame = nextAnimationFrame = nextBreathFrame = 0;
        pacer.Observe(0.0, 0.0, false);
    }

    // A held lens keeps showing live content at the idle rate.
    bool RefreshHeldLens(LONGLONG now) {
        if (!lens.visible || !nextLensFrame || now < nextLensFrame) return true;
        return RenderLens(lensPointer, false, true);
    }

    // Repaints a held spotlight whose settings changed while the pointer was hidden.
    bool RefreshHeldSpotlight() {
        if (!spotlight.visible || !SpotlightWanted() || highlightPixelsReady) return true;
        return RenderSpotlight(spotlight.lastPoint, false);
    }

    void HideAll() noexcept {
        stackWatching.store(false);
        CancelFrame();
        HidePointer();
        HideKeys();
        stackDirty = false;
        stackQueued.store(false);
        nextFrame = 0;
        nextStackCheck = 0;
    }

    void Fail(const std::wstring& text) noexcept {
        faulted = true;
        renderHealthy.store(false);
        HideAll();
        try { SetError(text); } catch (...) {}
    }

    void ApplyCaptureSetting() {
        bool ok = ApplyCaptureAffinity(highlight.window, config.captureVisible);
        for (auto& particle : particles) ok &= ApplyCaptureAffinity(particle.surface.window, config.captureVisible);
        for (auto& stamp : trail) ok &= ApplyCaptureAffinity(stamp.surface.window, config.captureVisible);
        ok &= ApplyCaptureAffinity(spotlight.window, config.captureVisible);
        ok &= ApplyCaptureAffinity(lens.host, config.captureVisible);
        ok &= ApplyCaptureAffinity(lens.frame.window, config.captureVisible);
        ok &= ApplyCaptureAffinity(keyPanel.surface.window, config.captureVisible);
        if (!ok && !config.captureVisible && !captureAffinityReported) {
            captureAffinityReported = true;
            SetError(L"Hiding effects from screen capture needs Windows 10 version 2004 or later; the effects stay visible in recordings on this system.");
        }
        if (config.captureVisible) captureAffinityReported = false;
    }

    BYTE BreathFade(LONGLONG now) const noexcept {
        if (!config.breathing || !breathOrigin) return 255;
        const double seconds = static_cast<double>(now - breathOrigin) / frequency;
        const double phase = 0.5 - 0.5 * std::cos(2.0 * kPi * seconds / kBreathPeriodSeconds);
        return static_cast<BYTE>(std::clamp(255.0 * (0.55 + 0.45 * phase) + 0.5, 0.0, 255.0));
    }

    bool RenderSpotlight(POINT point, bool raise) {
        std::wstring error;
        highlight.Hide(); // A halo from a previous style never shares the screen with it.
        const int radius = PixelRadius(config.spotlightRadius, monitorDpi);
        const BYTE alpha = ShadeAlpha(config.spotlightDim);
        if (!highlightPixelsReady || !spotlight.Configured(currentMonitorRect, radius, alpha)) {
            if (!spotlight.Prepare(controller.load(), config.captureVisible, error) ||
                !spotlight.Configure(currentMonitorRect, radius, alpha, error)) { Fail(error); return false; }
            highlightPixelsReady = true;
            highlightRasterizations.fetch_add(1, std::memory_order_relaxed);
            raise = true;
        } else if (!spotlight.visible || spotlight.lastPoint.x != point.x || spotlight.lastPoint.y != point.y) {
            highlightMoves.fetch_add(1, std::memory_order_relaxed);
        }
        if (!spotlight.Place(point, raise, error)) { Fail(error); return false; }
        // The dim layer was just raised: keep the lens and key display above it.
        if (raise) RaiseAboveSpotlight();
        return true;
    }

    void RaiseAboveSpotlight() noexcept {
        if (lens.visible) lens.Raise();
        if (keyPanel.visible && keyPanel.surface.window)
            SetWindowPos(keyPanel.surface.window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // Windows the lens leaves out of its enlarged image, most important first:
    // the halo, spotlight and key display, then the click rings and trail
    // stamps that overlap the enlarged area, newest first. Hidden windows and
    // ones elsewhere on the screen never appear in the image anyway.
    std::vector<HWND> LensExclusions(const RECT& source) const {
        std::vector<HWND> result{highlight.window, spotlight.window, keyPanel.surface.window};
        const auto overlaps = [&](const Surface& surface) {
            if (!surface.window || !surface.visible) return false;
            const RECT bounds{surface.lastOrigin.x, surface.lastOrigin.y,
                              surface.lastOrigin.x + surface.width, surface.lastOrigin.y + surface.height};
            RECT shared{};
            return IntersectRect(&shared, &bounds, &source) != FALSE;
        };
        std::vector<const Particle*> rings;
        for (const auto& particle : particles)
            if (particle.active && overlaps(particle.surface)) rings.push_back(&particle);
        std::sort(rings.begin(), rings.end(), [](const Particle* a, const Particle* b) { return a->started > b->started; });
        for (const Particle* particle : rings) result.push_back(particle->surface.window);
        for (std::size_t back = 1; back <= trail.size(); ++back) {
            const auto& stamp = trail[(nextTrail + 2 * trail.size() - back) % trail.size()];
            if (stamp.active && overlaps(stamp.surface)) result.push_back(stamp.surface.window);
        }
        return result;
    }

    // A magnifier problem disables only the lens, never the other effects.
    void LensFailed(const std::wstring& text) {
        lensUnavailable = true;
        HideLens();
        SetError(L"The magnifier is unavailable: " + text);
    }

    // held: the pointer is hidden (typing), so the lens keeps its place and
    // only refreshes what is under it.
    bool RenderLens(POINT point, bool raise = false, bool held = false) {
        if (!(held ? MayShowKeys() : MayShow()) || !LensWanted()) { HideLens(); return true; }
        std::wstring error;
        if (!magnification.Load(error)) { LensFailed(error); return true; }
        const int dpi = static_cast<int>(monitorDpi);
        const int sizePx = std::clamp(MulDiv(config.magnifierSize, dpi, 96), 40, 2400);
        const int border = std::max(2, MulDiv(3, dpi, 96));
        if (!lens.Prepare(controller.load(), sizePx, config.magnifierShape, border, config.magnifierBorder, config.magnifierFrame,
                          config.magnifierPosition != 0, config.captureVisible, error)) {
            LensFailed(error);
            return true;
        }
        const double zoom = config.magnifierZoom / 100.0;
        if (!lens.SetZoom(zoom, magnification)) {
            LensFailed(Failure(L"Windows rejected the magnifier's zoom level", GetLastError()));
            return true;
        }
        // Source square centered on the pointer: the child's side divided by
        // the zoom. It is kept inside the monitor so the edges of the screen
        // magnify instead of showing black.
        const int sourceSize = std::max(4, static_cast<int>(std::lround(lens.inner / zoom)));
        const RECT& monitor = currentMonitorRect;
        const LONG left = std::clamp<LONG>(point.x - sourceSize / 2, monitor.left, std::max(monitor.left, monitor.right - sourceSize));
        const LONG top = std::clamp<LONG>(point.y - sourceSize / 2, monitor.top, std::max(monitor.top, monitor.bottom - sourceSize));
        const RECT source{left, top, left + sourceSize, top + sourceSize};
        POINT center = point;
        if (config.magnifierPosition != 0) {
            // Beside: above and to the right, flipped when that leaves the monitor.
            const int offset = sizePx * 3 / 4;
            center.x = point.x + offset;
            center.y = point.y - offset;
            if (center.x + sizePx / 2 > monitor.right) center.x = point.x - offset;
            if (center.y - sizePx / 2 < monitor.top) center.y = point.y + offset;
        }
        if (!lens.Filter(LensExclusions(source), magnification)) {
            LensFailed(Failure(L"Windows rejected the magnifier's overlay filter", GetLastError()));
            return true;
        }
        if (!lens.Show(center, source, raise, magnification, error)) { LensFailed(error); return true; }
        lensPointer = point;
        lensVisible.store(true, std::memory_order_relaxed);
        magnifierUpdates.fetch_add(1, std::memory_order_relaxed);
        nextLensFrame = CounterNow() + std::max<LONGLONG>(1, frequency / kLensIdleRate);
        return true;
    }

    // Work area and DPI of the monitor that hosts the key display: the
    // foreground window's monitor, or the pointer's when there is none.
    RECT KeyMonitor(unsigned& dpi) const noexcept {
        HWND foreground = GetForegroundWindow();
        HMONITOR monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) : currentMonitor;
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!monitor || !GetMonitorInfoW(monitor, &info)) {
            dpi = monitorDpi;
            return currentMonitorRect;
        }
        UINT x = 96, y = 96;
        dpi = getDpiForMonitor && SUCCEEDED(getDpiForMonitor(monitor, 0, &x, &y)) ? std::clamp(x, 96u, 480u) : monitorDpi;
        return info.rcWork;
    }

    // Lays out and presents the key display for the current moment: rows
    // past their hold time fade, faded rows leave, and the frame timer is
    // armed for the next change.
    bool PaintKeyPanel(bool raise = false) {
        const LONGLONG now = CounterNow();
        const LONGLONG hold = frequency * config.keyDurationMs / 1000;
        const LONGLONG fadeLength = std::max<LONGLONG>(1, frequency * kKeyFadeMs / 1000);
        auto& strokes = keyPanel.strokes;
        strokes.erase(std::remove_if(strokes.begin(), strokes.end(),
            [&](const KeyStroke& stroke) { return now - stroke.updated >= hold + fadeLength; }), strokes.end());
        if (strokes.empty()) { HideKeys(); return true; }
        unsigned dpi = 96;
        const RECT work = KeyMonitor(dpi);
        std::wstring error;
        if (!keyPanel.EnsureFonts(KeyPanel::MetricsFor(dpi, config.keySize), error)) { Fail(error); return false; }
        std::vector<double> fades;
        LONGLONG next = 0;
        for (const auto& stroke : strokes) {
            const LONGLONG age = now - stroke.updated;
            double fade = 1.0;
            LONGLONG wake = stroke.updated + hold;
            if (age >= hold) {
                const double remaining = 1.0 - static_cast<double>(age - hold) / static_cast<double>(fadeLength);
                fade = remaining * remaining;
                wake = now + std::max<LONGLONG>(1, frequency / 60);
            }
            fades.push_back(fade);
            if (!next || wake < next) next = wake;
        }
        const int available = std::max(64, static_cast<int>(work.right - work.left) * 9 / 10);
        if (!keyPanel.Compose(config, available, fades, controller.load(), error)) { Fail(error); return false; }
        const int width = keyPanel.surface.width, height = keyPanel.surface.height;
        const int edge = MulDiv(40, static_cast<int>(dpi), 96);
        const int margin = keyPanel.metrics.margin;
        POINT origin{(work.left + work.right - width) / 2, work.bottom - edge - height + margin};
        switch (config.keyPosition) {
        case 1: origin.y = work.top + edge - margin; break;
        case 2: origin.x = work.left + edge - margin; break;
        case 3: origin.x = work.right - edge - width + margin; break;
        default: break;
        }
        if (!keyPanel.Present(origin, error, raise || !keyPanel.visible)) { Fail(error); return false; }
        keyVisible.store(true, std::memory_order_relaxed);
        keyDisplayUpdates.fetch_add(1, std::memory_order_relaxed);
        nextKeyFrame = next;
        return true;
    }

    void HandleKey(WPARAM packed, bool preview) {
        if (!MayShowKeys() || (!config.keyDisplay && !preview)) return;
        const unsigned virtualKey = static_cast<unsigned>(packed & 0xff);
        const unsigned scanCode = static_cast<unsigned>((packed >> 8) & 0xff);
        const unsigned modifiers = static_cast<unsigned>((packed >> 16) & 0xff);
        const bool extended = ((packed >> 24) & 1) != 0;
        HWND foreground = GetForegroundWindow();
        const HKL layout = GetKeyboardLayout(foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0);
        const LONGLONG now = CounterNow();
        const LONGLONG hold = frequency * config.keyDurationMs / 1000;
        auto& strokes = keyPanel.strokes;
        if (!keyPanel.visible) strokes.clear();
        KeyStroke* last = strokes.empty() ? nullptr : &strokes.back();
        // A typed row is still being written while it has not started to fade.
        const bool typing = last && !last->typed.empty() && now - last->updated < hold;
        const std::wstring typed = preview ? std::wstring() : TypedText(virtualKey, scanCode, modifiers, layout);
        const bool plain = !(modifiers & (kModControl | kModAlt | kModWin | kModMouse));
        if (typing && !typed.empty()) {
            last->typed += typed;
            if (last->typed.size() > kTypedLimit) last->typed.erase(0, last->typed.size() - kTypedLimit);
            last->updated = now;
            last->dirty = true;
        } else if (typing && plain && virtualKey == VK_BACK) {
            // Backspace edits the typed row, so it reads like the text itself.
            last->typed.pop_back();
            if (last->typed.empty()) strokes.pop_back();
            else { last->updated = now; last->dirty = true; }
        } else if (!typed.empty() && typed != L" ") {
            KeyStroke stroke;
            stroke.typed = typed;
            stroke.updated = now;
            strokes.push_back(std::move(stroke));
        } else {
            std::vector<std::wstring> caps = preview ? std::vector<std::wstring>{L"Ctrl", L"Shift", L"S"}
                                                     : KeyChordParts(virtualKey, scanCode, extended, modifiers, layout);
            if (last && last->caps == caps && now - last->updated < frequency) {
                ++last->repeat;
                last->updated = now;
                last->dirty = true;
            } else {
                KeyStroke stroke;
                stroke.caps = std::move(caps);
                stroke.updated = now;
                strokes.push_back(std::move(stroke));
            }
        }
        while (strokes.size() > kKeyRowLimit) strokes.erase(strokes.begin());
        if (!PaintKeyPanel()) return;
        FinishFrame();
    }

    // Holds each row for the configured time, then fades it out.
    bool UpdateKeyDisplay(LONGLONG) {
        if (!keyPanel.visible) { nextKeyFrame = 0; return true; }
        return PaintKeyPanel();
    }

    bool RenderHighlight(POINT point, bool raise = false) {
        if (!MayShow() || !HighlightWanted() || idleHidden) {
            HideHighlight();
            return true;
        }
        if (config.highlightStyle == kHighlightSpotlight) return RenderSpotlight(point, raise);
        spotlight.Hide();
        std::wstring error;
        const LONGLONG now = CounterNow();
        if (!highlightPixelsReady) {
            const int radius = PixelRadius(config.highlightRadius, monitorDpi);
            const double line = std::max(1.0, config.highlightThickness * monitorDpi / 96.0);
            // Odd dimensions place the circle's center exactly on the pointer,
            // including at fractional Windows scale factors after DIP rounding.
            if (!highlight.Prepare(radius * 2 + 7, controller.load(), config.captureVisible, error)) {
                Fail(error); return false;
            }
            switch (config.highlightStyle) {
            case kHighlightGlow: highlight.DrawGlow(radius, config.highlightColor, config.highlightOpacity); break;
            case kHighlightCrosshair: highlight.DrawCrosshair(radius, line, config.highlightColor, config.highlightOpacity); break;
            case kHighlightRing: highlight.DrawCircle(radius, line, false, config.highlightColor, config.highlightOpacity); break;
            default: highlight.DrawCircle(radius, line, true, config.highlightColor, config.highlightOpacity); break;
            }
            if (!breathOrigin) breathOrigin = now;
            if (!highlight.Present(point, BreathFade(now), error, raise)) { Fail(error); return false; }
            highlightPixelsReady = true;
            highlightRasterizations.fetch_add(1, std::memory_order_relaxed);
        } else {
            const bool moved = !highlight.visible || !highlight.positioned ||
                highlight.lastCenter.x != point.x || highlight.lastCenter.y != point.y;
            if (!highlight.Move(point, error, raise)) { Fail(error); return false; }
            if (moved) highlightMoves.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    // Re-presents the cached pixels with the current breathing fade. This is a
    // blend-only update: no rasterization and no window move.
    bool RenderBreath(LONGLONG now) {
        if (!config.breathing || !highlight.visible || config.highlightStyle == kHighlightSpotlight || !highlightPixelsReady) return true;
        std::wstring error;
        const BYTE fade = BreathFade(now);
        if (fade == highlight.lastFade) return true;
        if (!highlight.Present(highlight.lastCenter, fade, error)) { Fail(error); return false; }
        return true;
    }

    bool RenderTrail(POINT point, LONGLONG now) {
        if (!config.trail || !config.trailOpacity) { ClearTrail(); return true; }
        if (!trailPointReady || now - lastTrailStamp >
                frequency * config.trailDurationMs / 1000) {
            lastTrailPoint = point;
            lastTrailStamp = now;
            trailPointReady = true;
        } else {
            const double dx = static_cast<double>(point.x) - lastTrailPoint.x;
            const double dy = static_cast<double>(point.y) - lastTrailPoint.y;
            const double spacing = std::max(2.0, config.trailRadius * monitorDpi / 160.0);
            const LONGLONG sampleInterval = std::max<LONGLONG>(1,
                frequency * std::max(4, config.trailDurationMs / 30) / 1000);
            if (dx * dx + dy * dy >= spacing * spacing && now - lastTrailStamp >= sampleInterval) {
                auto& stamp = trail[nextTrail++ % trail.size()];
                stamp.origin = lastTrailPoint;
                stamp.started = now;
                const unsigned dpi = DpiAt(stamp.origin);
                const int radius = PixelRadius(config.trailRadius, dpi);
                std::wstring error;
                if (!stamp.surface.Prepare(radius * (config.trailStyle == 3 ? 4 : 2) + 7, controller.load(), config.captureVisible, error)) {
                    Fail(error); return false;
                }
                if (config.trailStyle == 2 || config.trailStyle == 4)
                    stamp.surface.DrawSparkle(radius, config.trailColor, config.trailOpacity, config.trailStyle == 4);
                else if (config.trailStyle == 3)
                    stamp.surface.DrawComet(radius, dx, dy, config.trailColor, config.trailOpacity);
                else stamp.surface.DrawCircle(radius, std::max(1.0, dpi / 96.0),
                    config.trailStyle == 0, config.trailColor, config.trailOpacity);
                if (!stamp.surface.Present(stamp.origin, 255, error)) { Fail(error); return false; }
                stamp.active = true;
                lastTrailPoint = point;
                lastTrailStamp = now;
            }
        }
        for (auto& stamp : trail) {
            if (!stamp.active) continue;
            const double fraction = static_cast<double>(now - stamp.started) * 1000.0 /
                                    (frequency * static_cast<double>(config.trailDurationMs));
            if (fraction >= 1.0) { stamp.active = false; stamp.surface.Hide(); continue; }
            const double remaining = std::clamp(1.0 - fraction, 0.0, 1.0);
            const BYTE alpha = static_cast<BYTE>(remaining * remaining * 255.0 + 0.5);
            std::wstring error;
            // Stamp pixels and positions are immutable; only their opacity fades.
            // A separate bounded pass handles desktop stacking changes.
            if (!stamp.surface.Present(stamp.origin, alpha, error)) {
                Fail(error); return false;
            }
        }
        return true;
    }

    void QueuePointer() noexcept {
        if (!accepting.load() || !allowed.load() || !renderHealthy.load() ||
            !pointerRequested.load()) return;
        pointerDirty.store(true);
        if (!pointerScheduled.exchange(true)) {
            const HWND window = controller.load();
            if (!window || !PostMessageW(window, kPointerKick, 0, 0))
                pointerScheduled.store(false);
        }
    }

    bool ArmFrame(LONGLONG deadline) {
        const LONGLONG now = CounterNow();
        // Past deadlines are serviced once, with current state. No replay or
        // sequence of overdue frames is ever enqueued.
        nextFrame = deadline;
        if (frameArmed && armedDeadline == deadline) return true;
        const LONGLONG remaining = std::max<LONGLONG>(1, deadline - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1,
            (remaining * 10000000LL + frequency - 1) / frequency);
        if (!SetWaitableTimerEx(frameTimer, &due, 0, nullptr, nullptr, nullptr, 0)) {
            const DWORD code = GetLastError();
            Fail(Failure(L"Could not schedule a pointer effect frame", code));
            return false;
        }
        armedDeadline = deadline;
        frameArmed = true;
        framePending.store(true, std::memory_order_relaxed);
        return true;
    }

    LONGLONG IdleHideDeadline() const noexcept {
        if (!config.idleHideMs || !HighlightVisible() || idleHidden || !lastPointerActivity) return 0;
        return lastPointerActivity + frequency * config.idleHideMs / 1000;
    }

    void FinishFrame() {
        if (!MayShow() && !MayShowKeys()) { HideAll(); return; }
        if (!MayShow()) PointerHidden();
        stackWatching.store(HighlightVisible() || AnyParticles() || AnyTrail() || lens.visible || keyPanel.visible);
        if (!pointerDirty.load()) pointerScheduled.store(false);
        // A producer may have set dirty while it still observed scheduled=true.
        // Recheck after releasing that flag so the final movement is never lost.
        if (pointerDirty.load()) pointerScheduled.store(true);
        LONGLONG deadline = 0;
        bool needed = false;
        auto include = [&](LONGLONG value) {
            if (!needed || value < deadline) deadline = value;
            needed = true;
        };
        if (MayShow()) {
            if (pointerDirty.load()) include(nextMotionFrame);
            if (AnyParticles() || AnyTrail() || trailMotionPending) include(nextAnimationFrame);
            if (const LONGLONG idle = IdleHideDeadline()) include(idle);
            if (config.breathing && highlight.visible && config.highlightStyle != kHighlightSpotlight) {
                // Breathing is a blend-only update; 30 Hz is plenty for a slow pulse.
                if (!nextBreathFrame) nextBreathFrame = CounterNow() + frequency / 30;
                include(nextBreathFrame);
            }
        }
        // The lens re-reads the screen at a modest rate while the pointer
        // rests or is hidden, so video and typing under it stay live.
        if (lens.visible && nextLensFrame) include(nextLensFrame);
        if (stackDirty) include(std::max(nextMotionFrame, nextStackCheck));
        if (keyPanel.visible && nextKeyFrame) include(nextKeyFrame);
        if (needed) ArmFrame(deadline);
        else CancelFrame();
    }

    static LONGLONG AdvanceDeadline(LONGLONG previous, LONGLONG now,
                                    LONGLONG interval) noexcept {
        return previous > 0 && now >= previous && now - previous < interval
            ? previous + interval : now + interval;
    }

    bool RepairStacking(LONGLONG now) {
        if (now < nextStackCheck) return true;
        // Desktop notifications are independent of render frequency. Keep their
        // metadata walks bounded even if an app emits a continuous event stream.
        nextStackCheck = now + std::max<LONGLONG>(1, frequency / 30);
        stackDirty = false;
        stackQueued.store(false);
        std::wstring error;
        // The dim layer covers the whole monitor: after raising it, our own
        // overlays go back above it.
        if (spotlight.RepairStack()) RaiseAboveSpotlight();
        if (!highlight.RepairStack(error)) { Fail(error); return false; }
        for (auto& particle : particles)
            if (particle.active && !particle.surface.RepairStack(error)) { Fail(error); return false; }
        for (auto& stamp : trail)
            if (stamp.active && !stamp.surface.RepairStack(error)) { Fail(error); return false; }
        if (lens.Covered()) lens.Raise();
        if (keyPanel.visible && !keyPanel.surface.RepairStack(error)) { Fail(error); return false; }
        return true;
    }

    bool DrawParticle(Particle& particle, LONGLONG now) {
        const double elapsedMs = static_cast<double>(std::max<LONGLONG>(0, now - particle.started)) *
                                 1000.0 / static_cast<double>(frequency);
        const double fraction = std::clamp(elapsedMs / particle.duration, 0.0, 1.0);
        if (fraction >= 1.0) {
            particle.active = false;
            particle.surface.Hide();
            return true;
        }
        const double remaining = 1.0 - fraction;
        double expansion = fraction;
        double fadeAmount = remaining;
        if (particle.easing == 1) {
            expansion = 1.0 - remaining * remaining * remaining;
            fadeAmount = remaining * remaining;
        } else if (particle.easing == 2) {
            expansion = fraction * fraction * (3.0 - 2.0 * fraction);
            fadeAmount = 1.0 - expansion;
        }
        const double radius = std::max(2.0, particle.radius * (0.28 + 0.72 * expansion));
        const double lineWidth = std::max(1.0, particle.thickness * particle.dpi / 96.0);
        switch (particle.style) {
        case kClickPulse: particle.surface.DrawCircle(radius, lineWidth, true, particle.color, particle.opacity); break;
        case kClickRipple: particle.surface.DrawRipple(radius, lineWidth, particle.color, particle.opacity); break;
        case kClickBurst: particle.surface.DrawBurst(particle.radius, lineWidth, particle.color, particle.opacity, expansion); break;
        case kClickNeon: particle.surface.DrawNeonRing(radius, lineWidth, particle.color, particle.opacity); break;
        case kClickTarget:
            // Contracts from the full radius to half of it, locking onto the click.
            particle.surface.DrawTarget(std::max(2.0, particle.radius * (1.0 - 0.5 * expansion)), lineWidth,
                                        particle.color, particle.opacity);
            break;
        default: particle.surface.DrawCircle(radius, lineWidth, false, particle.color, particle.opacity); break;
        }
        const BYTE fade = static_cast<BYTE>(std::clamp(255.0 * fadeAmount + 0.5, 0.0, 255.0));
        std::wstring error;
        if (!particle.surface.Present(particle.origin, fade, error)) {
            Fail(error);
            return false;
        }
        clickFrames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Pointer-following work for one frame. Returns false when the frame
    // ended early (a failure, or the pointer effects were hidden).
    bool RenderPointerFrame(LONGLONG now) {
        POINT point{};
        if (!VisibleCursor(point)) { PointerHidden(); RefreshHeldLens(now); return false; }
        UpdateMonitor(point);
        const bool move = pointerDirty.load() && now >= nextMotionFrame;
        if (move) { lastPointerActivity = now; idleHidden = false; }
        if (move || stackDirty) {
            if (move) {
                pointerDirty.exchange(false);
                if (config.trail && config.trailOpacity > 0) trailMotionPending = true;
                nextMotionFrame = AdvanceDeadline(nextMotionFrame, now, motionInterval);
            }
            // Cached pointer feedback is first. Sample immediately before its
            // move, after monitor work and before particle/trail rasterization.
            if (!VisibleCursor(point)) { PointerHidden(); RefreshHeldLens(now); return false; }
            UpdateMonitor(point);
            if (!RenderHighlight(point)) return false;
            if (move && !RenderLens(point)) return false;
        }
        if (const LONGLONG idle = IdleHideDeadline(); idle && now >= idle) {
            idleHidden = true;
            HideHighlight();
        }
        if (config.breathing && nextBreathFrame && now >= nextBreathFrame) {
            if (!RenderBreath(now)) return false;
            nextBreathFrame = AdvanceDeadline(nextBreathFrame, now, std::max<LONGLONG>(1, frequency / 30));
        }
        if (lens.visible && nextLensFrame && now >= nextLensFrame) {
            if (!VisibleCursor(point)) { PointerHidden(); RefreshHeldLens(now); return false; }
            if (!RenderLens(point)) return false;
        }
        if ((AnyParticles() || AnyTrail() || trailMotionPending) && now >= nextAnimationFrame) {
            const LONGLONG workStarted = CounterNow();
            const LONGLONG previous = lastAnimationFrame;
            lastAnimationFrame = workStarted;
            if (!VisibleCursor(point)) { PointerHidden(); RefreshHeldLens(now); return false; }
            if (!RenderTrail(point, workStarted)) return false;
            trailMotionPending = false;
            for (auto& particle : particles) {
                if (particle.active && !DrawParticle(particle, CounterNow())) return false;
            }
            const LONGLONG finished = CounterNow();
            const double elapsed = previous > 0 ?
                static_cast<double>(workStarted - previous) / frequency : 1.0 / pacer.Rate();
            if (pacer.Observe(static_cast<double>(finished - workStarted) / frequency, elapsed)) {
                const unsigned rate = pacer.Rate();
                effectiveRefreshRate.store(rate, std::memory_order_relaxed);
                frameInterval = std::max<LONGLONG>(1, (frequency + rate - 1) / rate);
            }
            nextAnimationFrame = AdvanceDeadline(nextAnimationFrame, workStarted, frameInterval);
            if (nextAnimationFrame <= finished) nextAnimationFrame = finished + frameInterval;
            // A costly particle pass must not leave the cached halo at its
            // pre-raster position until the next animation deadline. Auto keeps
            // this cheap final move at the display cap; a manual cap is strict.
            if (!config.refreshRate && pointerDirty.load() && finished >= nextMotionFrame) {
                pointerDirty.exchange(false);
                lastPointerActivity = finished; idleHidden = false;
                if (config.trail && config.trailOpacity > 0) trailMotionPending = true;
                if (!VisibleCursor(point)) { PointerHidden(); RefreshHeldLens(now); return false; }
                if (!RenderHighlight(point)) return false;
                if (!RenderLens(point)) return false;
                nextMotionFrame = AdvanceDeadline(nextMotionFrame, finished, motionInterval);
            }
        }
        return true;
    }

    void RenderFrame() {
        CancelFrame();
        if (!MayShow() && !MayShowKeys()) { HideAll(); return; }
        const LONGLONG now = CounterNow();
        frameCount.fetch_add(1, std::memory_order_relaxed);
        const bool repair = now >= nextStackCheck;
        if (MayShow()) {
            if (!RenderPointerFrame(now) && faulted) return;
        } else {
            PointerHidden();
            if (!RefreshHeldLens(now) && faulted) return;
        }
        if (keyPanel.visible && nextKeyFrame && now >= nextKeyFrame && !UpdateKeyDisplay(now)) return;
        // Every active pass can check at most 30 times/second; an idle halo has
        // no timer just for this. A new stacking event wakes a single pass.
        if (repair && !RepairStacking(CounterNow())) return;
        FinishFrame();
    }

    void RequestFrame() {
        if (!MayShow() && !MayShowKeys()) { HideAll(); return; }
        FinishFrame();
        if (frameArmed && CounterNow() >= nextFrame) RenderFrame();
    }

    void PointerKick() {
        pointerWakeups.fetch_add(1, std::memory_order_relaxed);
        if (!PointerWanted()) {
            pointerDirty.store(false);
            HideHighlight();
            HideLens();
            if (!AnyParticles() && !AnyTrail()) pointerScheduled.store(false);
            FinishFrame();
            return;
        }
        if (!MayShow()) { PointerHidden(); FinishFrame(); return; }
        RequestFrame();
    }

    void ApplySettings() {
        settingsQueued.store(false);
        const Config previous = config;
        {
            std::lock_guard<std::mutex> lock(configMutex);
            config = pending;
        }
        const bool recover = faulted;
        faulted = false;
        renderHealthy.store(true);
        SetError({});
        // A changed spotlight is repainted in place on the next render: one
        // window swaps its whole content at once, so nothing is hidden first.
        if (recover || HighlightAppearanceChanged(previous, config)) highlightPixelsReady = false;
        if (!SpotlightWanted()) spotlight.Reset(); // Frees the monitor-sized surface.
        if (recover || TrailAppearanceChanged(previous, config)) ClearTrail();
        if (previous.refreshRate != config.refreshRate) currentMonitor = nullptr;
        if (previous.captureVisible != config.captureVisible || recover) ApplyCaptureSetting();
        if (!config.breathing) nextBreathFrame = 0;
        if (!config.idleHideMs) idleHidden = false;
        // Any magnifier change is a fresh attempt, including after a failure.
        // A changed lens is adjusted in place by the render below.
        if (recover || MagnifierAppearanceChanged(previous, config) || previous.magnifier != config.magnifier) lensUnavailable = false;
        if (recover) lens.Reset();
        if (!config.magnifier) HideLens();
        if (!config.keyDisplay && keyPanel.visible) HideKeys();
        if (!MayShow() && !MayShowKeys()) { HideAll(); return; }
        if (keyPanel.visible && (recover || KeyAppearanceChanged(previous, config) || previous.captureVisible != config.captureVisible)) {
            for (auto& stroke : keyPanel.strokes) stroke.dirty = true;
            if (!PaintKeyPanel()) return;
        }
        if (!MayShow()) { PointerHidden(); if (!RefreshHeldSpotlight()) return; FinishFrame(); return; }
        if (!config.clicks || config.clickOpacity == 0) ClearParticles();
        POINT point{};
        if (!VisibleCursor(point)) { PointerHidden(); if (!RefreshHeldSpotlight()) return; FinishFrame(); return; }
        UpdateMonitor(point, recover || previous.refreshRate != config.refreshRate);
        // Keep the visible window and existing click particles while sliders or
        // unrelated settings change. A changed halo is repainted in place.
        if (!RenderHighlight(point)) return;
        if (!RenderLens(point)) return;
        if (AnyParticles() || AnyTrail() || pointerDirty.load()) RequestFrame();
        else FinishFrame();
    }

    void ApplyAllowed() {
        allowedQueued.store(false);
        if (!MayShow() && !MayShowKeys()) { HideAll(); return; }
        if (!MayShow()) { PointerHidden(); FinishFrame(); return; }
        POINT point{};
        if (!VisibleCursor(point)) { PointerHidden(); FinishFrame(); return; }
        UpdateMonitor(point);
        if (!RenderHighlight(point)) return;
        if (!RenderLens(point, true)) return;
        // Rule/foreground transitions request the same bounded repair pass as
        // desktop notifications. Never repeatedly force TOPMOST against a
        // privileged layer that remained above a previous repair attempt.
        stackDirty = HighlightVisible() || AnyParticles() || AnyTrail() || lens.visible || keyPanel.visible;
        RequestFrame();
    }

    bool SpawnParticle(unsigned button, POINT origin, LONGLONG started) {
        Particle* selected = nullptr;
        for (auto& particle : particles) {
            if (!particle.active) { selected = &particle; break; }
            if (!selected || particle.started < selected->started) selected = &particle;
        }
        if (!selected) return true;
        selected->origin = origin;
        selected->started = started;
        selected->dpi = DpiAt(origin);
        selected->radius = PixelRadius(config.clickRadius, selected->dpi);
        selected->duration = config.durationMs;
        selected->opacity = config.clickOpacity;
        selected->color = button == 1 ? config.leftColor : button == 2 ? config.rightColor : config.middleColor;
        selected->style = config.clickStyle;
        selected->thickness = config.clickThickness;
        selected->easing = config.easing;
        std::wstring error;
        const int padding = ParticlePadding(selected->style, std::max(1, MulDiv(selected->thickness, static_cast<int>(selected->dpi), 96)));
        if (!selected->surface.Prepare((selected->radius + padding) * 2 + 7, controller.load(), config.captureVisible, error)) {
            Fail(error);
            return false;
        }
        selected->active = true;
        return DrawParticle(*selected, CounterNow());
    }

    void HandleClick(std::size_t index) {
        if (index >= clicks.size()) return;
        auto& queued = clicks[index];
        if (queued.state.load(std::memory_order_acquire) != 2) return;
        const unsigned buttons = queued.buttons;
        const POINT point = queued.origin;
        const LONGLONG started = queued.started;
        queued.state.store(0, std::memory_order_release);
        const double ageMs = static_cast<double>(CounterNow() - started) * 1000.0 / frequency;
        if (!MayShow() || !config.clicks || config.clickOpacity == 0 ||
            ageMs >= config.durationMs) return;
        POINT current{};
        if (!VisibleCursor(current)) { PointerHidden(); FinishFrame(); return; }
        UpdateMonitor(current);
        const unsigned selected = buttons & config.buttons;
        if ((selected & 1) && !SpawnParticle(1, point, started)) return;
        if ((selected & 2) && !SpawnParticle(2, point, started)) return;
        if ((selected & 4) && !SpawnParticle(4, point, started)) return;
        if (selected && AnyParticles()) {
            // A click starts visibly now; continuing expansion/fade is paced.
            const LONGLONG now = CounterNow();
            if (!nextAnimationFrame || nextAnimationFrame <= now)
                nextAnimationFrame = now + frameInterval;
            FinishFrame();
        }
    }

    void SignalStart(bool success, const std::wstring& error) {
        {
            std::lock_guard<std::mutex> lock(startMutex);
            if (startComplete) return;
            startSuccess = success;
            startError = error;
            startComplete = true;
        }
        startCondition.notify_all();
    }

    static LRESULT CALLBACK ControllerProc(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        try {
            switch (message) {
            case kApplySettings: self->ApplySettings(); return 0;
            case kApplyAllowed: self->ApplyAllowed(); return 0;
            case kPointerKick: self->PointerKick(); return 0;
            case kClick: self->HandleClick(static_cast<std::size_t>(wParam)); return 0;
            case kKeyEvent: self->HandleKey(wParam, lParam != 0); return 0;
            case kStackChanged:
                self->stackWakeups.fetch_add(1, std::memory_order_relaxed);
                self->stackDirty = true;
                // Use the normal frame deadline to coalesce bursts. This also
                // rechecks pause rules/cursor visibility before showing anything.
                if ((self->MayShow() && (self->HighlightVisible() || self->AnyParticles() || self->AnyTrail() || self->lens.visible)) ||
                    (self->MayShowKeys() && (self->keyPanel.visible || self->spotlight.visible || self->lens.visible)))
                    self->RequestFrame();
                else { self->stackDirty = false; self->stackQueued.store(false); }
                return 0;
            case kMonitorChanged:
            case WM_DISPLAYCHANGE:
            case WM_DPICHANGED:
                self->highlightPixelsReady = false;
                self->currentMonitor = nullptr;
                if (message == WM_DISPLAYCHANGE) self->ClearParticles();
                self->pointerDirty.store(true);
                self->RequestFrame();
                return 0;
            case WM_CLOSE:
                self->HideAll();
                PostQuitMessage(0);
                return 0;
            default: return DefWindowProcW(window, message, wParam, lParam);
            }
        } catch (...) {
            self->Fail(L"The pointer effects renderer stopped after an unexpected error. Reapply settings to retry.");
            return 0;
        }
    }

    bool Initialize(std::wstring& error) {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        LARGE_INTEGER clockFrequency{};
        if (!QueryPerformanceFrequency(&clockFrequency) || clockFrequency.QuadPart <= 0) {
            error = L"Windows did not provide the clock needed for smooth pointer effects.";
            return false;
        }
        frequency = clockFrequency.QuadPart;
        frameInterval = std::max<LONGLONG>(1, (frequency + 59) / 60);
        motionInterval = frameInterval;
        frameTimer = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionTimer,
                                            TIMER_MODIFY_STATE | SYNCHRONIZE);
        highResolutionTimer.store(frameTimer != nullptr, std::memory_order_relaxed);
        if (!frameTimer)
            frameTimer = CreateWaitableTimerExW(nullptr, nullptr, 0,
                                                TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!frameTimer) {
            const DWORD code = GetLastError();
            error = Failure(L"Could not create the pointer effect frame timer", code);
            return false;
        }
        WNDCLASSEXW overlayClass{};
        overlayClass.cbSize = sizeof(overlayClass);
        overlayClass.hInstance = GetModuleHandleW(nullptr);
        overlayClass.lpfnWndProc = OverlayProc;
        overlayClass.lpszClassName = kOverlayClass;
        if (!RegisterClassExW(&overlayClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = Failure(L"Could not register pointer effect windows", GetLastError());
            return false;
        }
        WNDCLASSEXW lensClass{};
        lensClass.cbSize = sizeof(lensClass);
        lensClass.hInstance = GetModuleHandleW(nullptr);
        lensClass.lpfnWndProc = LensProc;
        lensClass.lpszClassName = kLensClass;
        if (!RegisterClassExW(&lensClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = Failure(L"Could not register the magnifier window", GetLastError());
            return false;
        }
        WNDCLASSEXW controllerClass{};
        controllerClass.cbSize = sizeof(controllerClass);
        controllerClass.hInstance = GetModuleHandleW(nullptr);
        controllerClass.lpfnWndProc = ControllerProc;
        controllerClass.lpszClassName = kControllerClass;
        if (!RegisterClassExW(&controllerClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = Failure(L"Could not register the pointer effect controller", GetLastError());
            return false;
        }
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kControllerClass, L"ClickShade effect controller", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!window) {
            error = Failure(L"Could not start the pointer effect controller", GetLastError());
            return false;
        }
        controller.store(window);
        // Delivery itself can be expensive even when a callback ignores the
        // event. Isolate global WinEvent traffic from the frame message queue.
        if (!stackListener.Start({{EVENT_OBJECT_SHOW, EVENT_OBJECT_REORDER},
                {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
                {EVENT_SYSTEM_MENUSTART, EVENT_SYSTEM_MENUPOPUPEND}},
                StackEvent, this, error)) return false;
        shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (shcore) {
            const FARPROC address = GetProcAddress(shcore, "GetDpiForMonitor");
            static_assert(sizeof(address) == sizeof(getDpiForMonitor));
            std::memcpy(&getDpiForMonitor, &address, sizeof(address));
        }
        {
            std::lock_guard<std::mutex> lock(configMutex);
            config = pending;
        }
        PublishRequested(config);
        accepting.store(true);
        // Rules default to disallowed until the input thread checks foreground,
        // fullscreen, confinement, desktop/session, and cursor visibility.
        return true;
    }

    void Cleanup() noexcept {
        accepting.store(false);
        allowed.store(false);
        stackWatching.store(false);
        stackListener.Stop();
        stackQueued.store(false);
        HideAll();
        for (auto& particle : particles) particle.surface.Reset();
        for (auto& stamp : trail) stamp.surface.Reset();
        highlight.Reset();
        spotlight.Reset();
        keyPanel.Reset();
        lens.Reset(); // Before MagUninitialize: the lens child belongs to the API.
        magnification.Unload();
        if (frameTimer) CloseHandle(frameTimer);
        frameTimer = nullptr;
        const HWND window = controller.exchange(nullptr);
        if (window) DestroyWindow(window);
        if (shcore) FreeLibrary(shcore);
        shcore = nullptr;
        getDpiForMonitor = nullptr;
        for (auto& queued : clicks) queued.state.store(0);
    }

    void ThreadMain() noexcept {
        threadId.store(GetCurrentThreadId());
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        try {
            std::wstring error;
            if (!Initialize(error)) {
                SetError(error);
                SignalStart(false, error);
            } else if (stopping.load()) {
                SignalStart(false, L"Pointer effects startup was cancelled.");
            } else {
                SignalStart(true, {});
                bool quit = false;
                while (!quit && !stopping.load()) {
                    // Bound command processing so a burst of clicks/settings
                    // cannot starve an already-due motion/animation frame.
                    for (unsigned count = 0; count < 32 &&
                         PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                        if (message.message == WM_QUIT) { quit = true; break; }
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                        if (frameArmed && CounterNow() >= nextFrame) break;
                    }
                    if (quit || stopping.load()) break;
                    if (frameArmed && CounterNow() >= nextFrame) { RenderFrame(); continue; }
                    const DWORD handles = frameArmed ? 1u : 0u;
                    const DWORD result = MsgWaitForMultipleObjectsEx(
                        handles, handles ? &frameTimer : nullptr, INFINITE,
                        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    if (result == WAIT_FAILED) {
                        Fail(Failure(L"The pointer effects message wait failed", GetLastError()));
                        break;
                    }
                    if (handles && result == WAIT_OBJECT_0) {
                        frameArmed = false;
                        framePending.store(false, std::memory_order_relaxed);
                        RequestFrame();
                    }
                }
            }
        } catch (...) {
            Fail(L"Pointer effects could not start or continue.");
            try { SignalStart(false, L"Pointer effects could not start or continue."); } catch (...) {}
        }
        Cleanup();
        threadId.store(0);
    }
};

VisualEffects::VisualEffects() : impl_(std::make_unique<Impl>()) {}
VisualEffects::~VisualEffects() { Stop(); }

bool VisualEffects::Start(const Settings& settings, std::wstring& error) {
    Stop();
    impl_ = std::make_unique<Impl>();
    impl_->pending = MakeConfig(settings);
    try { impl_->worker = std::thread([this] { impl_->ThreadMain(); }); }
    catch (...) { error = L"Could not create the pointer effects thread."; return false; }
    std::unique_lock<std::mutex> lock(impl_->startMutex);
    const bool completed = impl_->startCondition.wait_for(lock, std::chrono::seconds(8),
        [this] { return impl_->startComplete; });
    const bool success = completed && impl_->startSuccess;
    error = completed ? impl_->startError : L"The pointer effects renderer did not finish starting.";
    lock.unlock();
    if (!success) Stop();
    return success;
}

void VisualEffects::Stop() {
    if (!impl_ || !impl_->worker.joinable()) return;
    impl_->stopping.store(true);
    impl_->accepting.store(false);
    impl_->allowed.store(false);
    impl_->keyAllowed.store(false);
    impl_->pointerRequested.store(false);
    impl_->keyRequested.store(false);
    impl_->clickButtons.store(0);
    const HWND window = impl_->controller.load();
    if (!window || !PostMessageW(window, WM_CLOSE, 0, 0)) {
        const DWORD thread = impl_->threadId.load();
        if (thread) PostThreadMessageW(thread, WM_QUIT, 0, 0);
    }
    impl_->worker.join();
}

void VisualEffects::UpdateSettings(const Settings& settings) {
    const Config config = MakeConfig(settings);
    {
        std::lock_guard<std::mutex> lock(impl_->configMutex);
        impl_->pending = config;
    }
    impl_->PublishRequested(config);
    if (!impl_->settingsQueued.exchange(true)) {
        const HWND window = impl_->controller.load();
        if (!window || !PostMessageW(window, kApplySettings, 0, 0))
            impl_->settingsQueued.store(false);
    }
}

void VisualEffects::SetAllowed(bool allowed) { SetAllowed(allowed, allowed); }

void VisualEffects::SetAllowed(bool allowed, bool keyDisplayAllowed) {
    impl_->allowed.store(allowed);
    impl_->keyAllowed.store(keyDisplayAllowed);
    if (!impl_->allowedQueued.exchange(true)) {
        const HWND window = impl_->controller.load();
        if (!window || !PostMessageW(window, kApplyAllowed, 0, 0))
            impl_->allowedQueued.store(false);
    }
}

void VisualEffects::PointerActivity() noexcept { impl_->QueuePointer(); }

void VisualEffects::Key(unsigned virtualKey, unsigned scanCode, bool extended, unsigned modifiers) noexcept {
    if (!impl_->accepting.load() || !impl_->keyAllowed.load() || !impl_->renderHealthy.load() ||
        !impl_->keyRequested.load()) return;
    const HWND window = impl_->controller.load();
    if (!window) return;
    const WPARAM packed = static_cast<WPARAM>(virtualKey & 0xffu) | (static_cast<WPARAM>(scanCode & 0xffu) << 8) |
                          (static_cast<WPARAM>(modifiers & 0xffu) << 16) | (extended ? (static_cast<WPARAM>(1) << 24) : 0);
    PostMessageW(window, kKeyEvent, packed, 0);
}

void VisualEffects::PreviewKey() noexcept {
    if (!impl_->accepting.load() || !impl_->renderHealthy.load()) return;
    const HWND window = impl_->controller.load();
    if (window) PostMessageW(window, kKeyEvent, 0, 1);
}

void VisualEffects::Click(unsigned buttonMask) noexcept {
    if (!impl_->accepting.load() || !impl_->allowed.load() ||
        !impl_->renderHealthy.load()) return;
    buttonMask &= impl_->clickButtons.load();
    if (!buttonMask) return;
    POINT point{};
    if (!VisibleCursor(point)) { impl_->QueuePointer(); return; }
    const HWND window = impl_->controller.load();
    if (!window) return;
    const unsigned begin = impl_->clickSequence.fetch_add(1);
    for (std::size_t offset = 0; offset < impl_->clicks.size(); ++offset) {
        const std::size_t index = (begin + offset) % impl_->clicks.size();
        auto& queued = impl_->clicks[index];
        unsigned free = 0;
        if (!queued.state.compare_exchange_strong(free, 1, std::memory_order_acquire)) continue;
        queued.buttons = buttonMask;
        queued.origin = point;
        queued.started = CounterNow();
        queued.state.store(2, std::memory_order_release);
        if (!PostMessageW(window, kClick, static_cast<WPARAM>(index), 0))
            queued.state.store(0, std::memory_order_release);
        return;
    }
    // A busy renderer drops excess visual clicks instead of queuing an
    // unbounded backlog or making the input thread wait.
}

void VisualEffects::SetNotifyWindow(HWND window, UINT message) noexcept {
    impl_->notifyMessage.store(message);
    impl_->notifyWindow.store(window);
}

std::wstring VisualEffects::LastError() const {
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    return impl_->errorText;
}

VisualEffectsStats VisualEffects::Stats() const noexcept {
    VisualEffectsStats result;
    result.frameCount = impl_->frameCount.load(std::memory_order_relaxed);
    result.highlightMoves = impl_->highlightMoves.load(std::memory_order_relaxed);
    result.highlightRasterizations = impl_->highlightRasterizations.load(std::memory_order_relaxed);
    result.clickFrames = impl_->clickFrames.load(std::memory_order_relaxed);
    result.pointerWakeups = impl_->pointerWakeups.load(std::memory_order_relaxed);
    result.stackWakeups = impl_->stackWakeups.load(std::memory_order_relaxed);
    result.magnifierUpdates = impl_->magnifierUpdates.load(std::memory_order_relaxed);
    result.keyDisplayUpdates = impl_->keyDisplayUpdates.load(std::memory_order_relaxed);
    result.magnifierVisible = impl_->lensVisible.load(std::memory_order_relaxed);
    result.keyDisplayVisible = impl_->keyVisible.load(std::memory_order_relaxed);
    result.targetRefreshRate = impl_->effectiveRefreshRate.load(std::memory_order_relaxed);
    result.motionRefreshRate = impl_->effectiveMotionRate.load(std::memory_order_relaxed);
    result.monitorDpi = impl_->effectiveDpi.load(std::memory_order_relaxed);
    result.highResolutionTimer = impl_->highResolutionTimer.load(std::memory_order_relaxed);
    result.framePending = impl_->framePending.load(std::memory_order_relaxed);
    return result;
}

}  // namespace clickshade
