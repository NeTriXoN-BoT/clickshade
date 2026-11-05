#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <vector>

namespace clickshade {

// Applications hide the pointer in several ways: ShowCursor or a null cursor
// (Chromium browsers), touch and pen suppression, or a cursor whose image is
// completely transparent (Firefox, for CSS cursor:none such as YouTube's
// idle player). All of them mean the user cannot see the pointer.
namespace pointer_detail {

inline bool ImageBlank(HCURSOR cursor) noexcept {
    ICONINFO info{};
    if (!cursor || !GetIconInfo(cursor, &info)) return false;
    bool blank = true;
    HDC dc = GetDC(nullptr);
    BITMAP mask{};
    if (!dc || !info.hbmMask || !GetObjectW(info.hbmMask, sizeof(mask), &mask) || mask.bmWidth <= 0 ||
        mask.bmHeight <= 0 || mask.bmWidth > 1024 || mask.bmHeight > 2048) {
        blank = false;
    } else {
        const int width = mask.bmWidth;
        const int height = info.hbmColor ? mask.bmHeight : mask.bmHeight / 2;
        bool alpha = false;
        if (info.hbmColor) {
            BITMAPINFO bits{};
            bits.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bits.bmiHeader.biWidth = width; bits.bmiHeader.biHeight = -height;
            bits.bmiHeader.biPlanes = 1; bits.bmiHeader.biBitCount = 32; bits.bmiHeader.biCompression = BI_RGB;
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
            if (GetDIBits(dc, info.hbmColor, 0, height, pixels.data(), &bits, DIB_RGB_COLORS) != height) {
                blank = false;
            } else {
                for (std::size_t i = 3; i < pixels.size(); i += 4) alpha |= pixels[i] != 0;
                // Alpha artwork is visible wherever alpha is set; legacy color
                // artwork (no alpha) shows any colored pixel, as XOR or opaque.
                if (alpha) blank = false;
                else for (std::size_t i = 0; i < pixels.size() && blank; i += 4)
                    blank = !(pixels[i] | pixels[i + 1] | pixels[i + 2]);
            }
        }
        if (blank && !alpha) {
            struct { BITMAPINFOHEADER header; RGBQUAD colors[2]; } monochrome{};
            monochrome.header.biSize = sizeof(BITMAPINFOHEADER);
            monochrome.header.biWidth = width; monochrome.header.biHeight = -mask.bmHeight;
            monochrome.header.biPlanes = 1; monochrome.header.biBitCount = 1; monochrome.header.biCompression = BI_RGB;
            const std::size_t stride = static_cast<std::size_t>((width + 31) / 32 * 4);
            std::vector<std::uint8_t> planes(stride * mask.bmHeight);
            if (GetDIBits(dc, info.hbmMask, 0, mask.bmHeight, planes.data(),
                          reinterpret_cast<BITMAPINFO*>(&monochrome), DIB_RGB_COLORS) != mask.bmHeight) {
                blank = false;
            } else {
                for (int y = 0; y < height && blank; ++y) for (int x = 0; x < width && blank; ++x) {
                    const bool transparent = (planes[y * stride + x / 8] >> (7 - x % 8)) & 1;
                    // An opaque pixel, or (monochrome) an inverting XOR pixel, is visible.
                    const bool inverts = !info.hbmColor && ((planes[(y + height) * stride + x / 8] >> (7 - x % 8)) & 1);
                    blank = transparent && !inverts;
                }
            }
        }
    }
    if (dc) ReleaseDC(nullptr, dc);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return blank;
}

}  // namespace pointer_detail

// True when the cursor image has no visible pixel at all. Results are cached
// per handle for a short time, so per-frame callers stay cheap; a reused
// handle value is re-examined after the cache entry expires.
inline bool CursorImageBlank(HCURSOR cursor) noexcept {
    struct Entry { HCURSOR cursor = nullptr; ULONGLONG checked = 0; bool blank = false; };
    thread_local std::array<Entry, 4> cache{};
    thread_local unsigned next = 0;
    if (!cursor) return true;
    const ULONGLONG now = GetTickCount64();
    for (auto& entry : cache)
        if (entry.cursor == cursor && now - entry.checked < 2000) return entry.blank;
    Entry& slot = cache[next++ % cache.size()];
    slot.cursor = cursor; slot.checked = now; slot.blank = pointer_detail::ImageBlank(cursor);
    return slot.blank;
}

inline bool PointerVisible(const CURSORINFO& info) noexcept {
    return (info.flags & CURSOR_SHOWING) && !(info.flags & CURSOR_SUPPRESSED) && info.hCursor &&
           !CursorImageBlank(info.hCursor);
}

}  // namespace clickshade
