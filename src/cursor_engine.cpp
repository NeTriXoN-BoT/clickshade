#include "cursor_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace clickshade {

void InvertCursorBgra(std::uint8_t* pixels, std::size_t count) noexcept {
    if (!pixels) return;
    for (std::size_t i = 0; i < count; ++i, pixels += 4) {
        if (!pixels[3]) continue;
        for (unsigned channel = 0; channel < 3; ++channel)
            pixels[channel] = static_cast<std::uint8_t>(255u - pixels[channel]);
    }
}

namespace {

// Artwork is filtered and styled in premultiplied space, then converted back
// to the straight alpha Windows expects before a cursor is created.
void PremultiplyBgra(std::uint8_t* pixels, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i, pixels += 4) {
        const unsigned a = pixels[3];
        for (unsigned channel = 0; channel < 3; ++channel)
            pixels[channel] = a ? static_cast<std::uint8_t>((pixels[channel] * a + 127u) / 255u) : 0;
    }
}

void UnpremultiplyBgra(std::uint8_t* pixels, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i, pixels += 4) {
        const unsigned a = pixels[3];
        for (unsigned channel = 0; channel < 3; ++channel)
            pixels[channel] = a ? static_cast<std::uint8_t>((std::min)(255u, (pixels[channel] * 255u + a / 2) / a)) : 0;
    }
}

constexpr DWORD kNormalCursor = 32512;
constexpr UINT kImageCursor = 2;

struct Role {
    DWORD id;
    const wchar_t* registryName;
};

// Every system cursor role, so a chosen size applies to all of them. Wait and
// AppStarting are usually animated: copying an animation flattens it, so
// those are reloaded from their file at the requested size and never
// inverted or snapshotted (see CursorSlot::animated).
constexpr DWORD kWaitCursor = 32514;
constexpr DWORD kAppStartingCursor = 32650;
constexpr Role kRoles[] = {
    {32512, L"Arrow"}, {32513, L"IBeam"}, {kWaitCursor, L"Wait"}, {32515, L"Crosshair"},
    {32516, L"UpArrow"}, {32631, L"NWPen"}, {32642, L"SizeNWSE"}, {32643, L"SizeNESW"},
    {32644, L"SizeWE"}, {32645, L"SizeNS"}, {32646, L"SizeAll"},
    {32648, L"No"}, {32649, L"Hand"}, {kAppStartingCursor, L"AppStarting"}, {32651, L"Help"},
    {32671, L"Pin"}, {32672, L"Person"},
};

std::wstring WinError(const wchar_t* action, DWORD code) {
    if (!code) code = ERROR_GEN_FAILURE;
    std::wstring message(action);
    message += L" (Windows error " + std::to_wstring(code) + L")";
    wchar_t* detail = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&detail), 0, nullptr);
    if (length && detail) {
        std::wstring text(detail, length);
        LocalFree(detail);
        while (!text.empty() && iswspace(text.back())) text.pop_back();
        if (!text.empty()) message += L": " + text;
    } else if (detail) {
        LocalFree(detail);
    }
    return message;
}

HCURSOR CopyOwnedCursor(HCURSOR source) noexcept {
    return reinterpret_cast<HCURSOR>(
        CopyImage(source, kImageCursor, 0, 0, 0));
}

std::wstring RegisteredCursorPath(const wchar_t* registryName) {
    DWORD bytes = 0;
    const wchar_t* key = L"Control Panel\\Cursors";
    if (RegGetValueW(HKEY_CURRENT_USER, key, registryName,
                    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                    nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes > 65536) {
        return {};
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key, registryName,
                    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                    nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    const DWORD expanded = ExpandEnvironmentStringsW(buffer.data(), nullptr, 0);
    if (!expanded || expanded > 32768) return {};
    std::vector<wchar_t> path(expanded, L'\0');
    if (!ExpandEnvironmentStringsW(buffer.data(), path.data(), expanded)) return {};
    return std::wstring(path.data());
}

struct ScreenDC {
    HDC value = GetDC(nullptr);
    ScreenDC() = default;
    ScreenDC(const ScreenDC&) = delete;
    ScreenDC& operator=(const ScreenDC&) = delete;
    ~ScreenDC() { if (value) ReleaseDC(nullptr, value); }
};

struct BitmapOwner {
    HBITMAP value = nullptr;
    BitmapOwner() = default;
    BitmapOwner(const BitmapOwner&) = delete;
    BitmapOwner& operator=(const BitmapOwner&) = delete;
    ~BitmapOwner() { if (value) DeleteObject(value); }
};

struct CursorOwner {
    HCURSOR value = nullptr;
    CursorOwner() = default;
    CursorOwner(const CursorOwner&) = delete;
    CursorOwner& operator=(const CursorOwner&) = delete;
    ~CursorOwner() { if (value) DestroyCursor(value); }
};

struct IconBitmaps {
    ICONINFO info{};
    IconBitmaps() = default;
    IconBitmaps(const IconBitmaps&) = delete;
    IconBitmaps& operator=(const IconBitmaps&) = delete;
    ~IconBitmaps() {
        if (info.hbmMask) DeleteObject(info.hbmMask);
        if (info.hbmColor) DeleteObject(info.hbmColor);
    }
};

struct MonoInfo {
    BITMAPINFOHEADER header{};
    RGBQUAD palette[2]{};
    MonoInfo(int width, int height) {
        header.biSize = sizeof(BITMAPINFOHEADER);
        header.biWidth = width;
        header.biHeight = -height;
        header.biPlanes = 1;
        header.biBitCount = 1;
        header.biCompression = BI_RGB;
        header.biClrUsed = 2;
        palette[1].rgbBlue = 255;
        palette[1].rgbGreen = 255;
        palette[1].rgbRed = 255;
    }
    BITMAPINFO* Get() { return reinterpret_cast<BITMAPINFO*>(this); }
};

bool ValidSize(const BITMAP& bitmap) noexcept {
    // A generous ceiling also bounds all stride and allocation calculations.
    return bitmap.bmWidth > 0 && bitmap.bmWidth <= 4096 &&
           bitmap.bmHeight > 0 && bitmap.bmHeight <= 8192;
}

std::size_t MonoStride(int width) noexcept {
    return (static_cast<std::size_t>(width) + 31) / 32 * 4;
}

bool MaskBit(const std::vector<std::uint8_t>& bits, std::size_t stride,
             int x, int y) noexcept {
    return (bits[static_cast<std::size_t>(y) * stride +
                 static_cast<std::size_t>(x) / 8] & (0x80u >> (x % 8))) != 0;
}

HCURSOR CreateInvertedCursor(HCURSOR source, std::wstring& error) {
    IconBitmaps sourceBits;
    if (!GetIconInfo(source, &sourceBits.info)) {
        const DWORD code = GetLastError();
        error = WinError(L"Could not read the cursor image", code);
        return nullptr;
    }
    BITMAP maskDescription{};
    if (!sourceBits.info.hbmMask ||
        GetObjectW(sourceBits.info.hbmMask, sizeof(maskDescription), &maskDescription) !=
            sizeof(maskDescription) || !ValidSize(maskDescription)) {
        error = L"The cursor has an unsupported transparency mask.";
        return nullptr;
    }
    ScreenDC dc;
    if (!dc.value) {
        const DWORD code = GetLastError();
        error = WinError(L"Could not obtain a cursor graphics context", code);
        return nullptr;
    }
    const int width = maskDescription.bmWidth;
    const int maskHeight = maskDescription.bmHeight;
    const std::size_t maskStride = MonoStride(width);
    MonoInfo maskInfo(width, maskHeight);
    std::vector<std::uint8_t> maskPixels(maskStride * maskHeight, 0);
    if (GetDIBits(dc.value, sourceBits.info.hbmMask, 0,
                  static_cast<UINT>(maskHeight), maskPixels.data(),
                  maskInfo.Get(), DIB_RGB_COLORS) != maskHeight) {
        const DWORD code = GetLastError();
        error = WinError(L"Could not read cursor transparency", code);
        return nullptr;
    }

    ICONINFO targetInfo = sourceBits.info;
    targetInfo.fIcon = FALSE;
    BitmapOwner targetMask;
    BitmapOwner targetColor;

    if (!sourceBits.info.hbmColor) {
        // Monochrome cursors have two stacked one-bit planes. Leave AND=1
        // pixels alone (transparent or background-inverting) and flip only
        // opaque black/white XOR pixels. Row padding stays unchanged.
        if ((maskHeight % 2) != 0) {
            error = L"The monochrome cursor has an invalid mask height.";
            return nullptr;
        }
        const int height = maskHeight / 2;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (!MaskBit(maskPixels, maskStride, x, y)) {
                    maskPixels[static_cast<std::size_t>(y + height) * maskStride +
                               static_cast<std::size_t>(x) / 8] ^=
                        static_cast<std::uint8_t>(0x80u >> (x % 8));
                }
            }
        }
        targetMask.value = CreateBitmap(width, maskHeight, 1, 1, nullptr);
        if (!targetMask.value) {
            const DWORD code = GetLastError();
            error = WinError(L"Could not create a monochrome cursor mask", code);
            return nullptr;
        }
        if (SetDIBits(dc.value, targetMask.value, 0, static_cast<UINT>(maskHeight),
                      maskPixels.data(), maskInfo.Get(), DIB_RGB_COLORS) != maskHeight) {
            const DWORD code = GetLastError();
            error = WinError(L"Could not write a monochrome cursor mask", code);
            return nullptr;
        }
        targetInfo.hbmMask = targetMask.value;
        targetInfo.hbmColor = nullptr;
    } else {
        BITMAP colorDescription{};
        if (GetObjectW(sourceBits.info.hbmColor, sizeof(colorDescription),
                       &colorDescription) != sizeof(colorDescription) ||
            !ValidSize(colorDescription) || colorDescription.bmWidth != width ||
            colorDescription.bmHeight != maskHeight) {
            error = L"The cursor has unsupported image dimensions.";
            return nullptr;
        }
        const int height = colorDescription.bmHeight;
        const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
        BITMAPINFO colorInfo{};
        colorInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        colorInfo.bmiHeader.biWidth = width;
        colorInfo.bmiHeader.biHeight = -height;
        colorInfo.bmiHeader.biPlanes = 1;
        colorInfo.bmiHeader.biBitCount = 32;
        colorInfo.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint8_t> pixels(pixelCount * 4, 0);
        if (GetDIBits(dc.value, sourceBits.info.hbmColor, 0,
                      static_cast<UINT>(height), pixels.data(), &colorInfo,
                      DIB_RGB_COLORS) != height) {
            const DWORD code = GetLastError();
            error = WinError(L"Could not read cursor colour pixels", code);
            return nullptr;
        }
        bool hasAlpha = false;
        for (std::size_t i = 0; i < pixelCount; ++i) hasAlpha |= pixels[i * 4 + 3] != 0;
        if (hasAlpha) {
            InvertCursorBgra(pixels.data(), pixelCount);
        } else {
            // All-zero alpha is a legacy AND/XOR cursor, not an invisible
            // alpha cursor. Invert opaque colour pixels, retaining the AND
            // plane and all transparent/background-XOR pixels exactly.
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    if (MaskBit(maskPixels, maskStride, x, y)) continue;
                    auto* pixel = pixels.data() +
                        (static_cast<std::size_t>(y) * width + x) * 4;
                    pixel[0] ^= 0xff;
                    pixel[1] ^= 0xff;
                    pixel[2] ^= 0xff;
                }
            }
        }
        void* destination = nullptr;
        targetColor.value = CreateDIBSection(dc.value, &colorInfo, DIB_RGB_COLORS,
                                             &destination, nullptr, 0);
        if (!targetColor.value || !destination) {
            const DWORD code = GetLastError();
            error = WinError(L"Could not create the dark cursor bitmap", code);
            return nullptr;
        }
        CopyMemory(destination, pixels.data(), pixels.size());
        targetInfo.hbmColor = targetColor.value;
        // CreateIconIndirect copies the mask, so the original remains intact.
    }
    const HCURSOR cursor = reinterpret_cast<HCURSOR>(CreateIconIndirect(&targetInfo));
    if (!cursor) {
        const DWORD code = GetLastError();
        error = WinError(L"Could not create the dark cursor", code);
    }
    return cursor;
}

bool CursorDimensions(HCURSOR cursor, int& width, int& height, DWORD& hotX,
                      DWORD& hotY, std::wstring& error) {
    IconBitmaps bits;
    BITMAP description{};
    if (!GetIconInfo(cursor, &bits.info) ||
        !GetObjectW(bits.info.hbmColor ? bits.info.hbmColor : bits.info.hbmMask,
                    sizeof(description), &description) || !ValidSize(description)) {
        error = L"Could not read the cursor dimensions.";
        return false;
    }
    width = description.bmWidth;
    height = bits.info.hbmColor ? description.bmHeight : description.bmHeight / 2;
    hotX = bits.info.xHotspot; hotY = bits.info.yHotspot;
    if (height < 1 || hotX >= static_cast<DWORD>(width) || hotY >= static_cast<DWORD>(height)) {
        error = L"The cursor hotspot is outside its image.";
        return false;
    }
    return true;
}

// True background-XOR pixels cannot be represented by alpha. Sample both
// legacy planes at the same pixel center rather than independently stretching
// them through GDI, so opaque/transparent/inverting states stay paired.
HCURSOR ResizeXorCursor(const ICONINFO& source, const std::vector<std::uint8_t>& colors,
                        const std::vector<std::uint8_t>& sourceMask, int width, int height,
                        int targetWidth, int targetHeight, DWORD hotX, DWORD hotY,
                        HDC dc, std::wstring& error) {
    const bool monochrome = source.hbmColor == nullptr;
    const auto sourceStride = MonoStride(width), stride = MonoStride(targetWidth);
    const int maskHeight = monochrome ? targetHeight * 2 : targetHeight;
    std::vector<std::uint8_t> maskPixels(stride * maskHeight, 0);
    BitmapOwner mask, color;
    mask.value = CreateBitmap(targetWidth, maskHeight, 1, 1, nullptr);
    std::uint8_t* output = nullptr;
    if (!monochrome) {
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = targetWidth; info.bmiHeader.biHeight = -targetHeight;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        color.value = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        output = static_cast<std::uint8_t*>(pixels);
    }
    if (!mask.value || (!monochrome && (!color.value || !output))) {
        error = WinError(L"Could not allocate resized cursor masks", GetLastError()); return nullptr;
    }
    for (int y = 0; y < targetHeight; ++y) for (int x = 0; x < targetWidth; ++x) {
        const int sx = std::min(width - 1, static_cast<int>((static_cast<std::int64_t>(x) * 2 + 1) * width / (targetWidth * 2)));
        const int sy = std::min(height - 1, static_cast<int>((static_cast<std::int64_t>(y) * 2 + 1) * height / (targetHeight * 2)));
        const auto offset = static_cast<std::size_t>(y) * stride + x / 8;
        const auto bit = static_cast<std::uint8_t>(0x80u >> (x % 8));
        if (MaskBit(sourceMask, sourceStride, sx, sy)) maskPixels[offset] |= bit;
        if (monochrome) {
            if (MaskBit(sourceMask, sourceStride, sx, sy + height))
                maskPixels[offset + stride * targetHeight] |= bit;
        } else {
            auto* destination = output + (static_cast<std::size_t>(y) * targetWidth + x) * 4;
            const auto* pixel = colors.data() + (static_cast<std::size_t>(sy) * width + sx) * 4;
            std::copy_n(pixel, 3, destination);
            destination[3] = 0; // Preserve legacy AND/XOR rendering, not alpha blending.
        }
    }
    MonoInfo maskInfo(targetWidth, maskHeight);
    if (SetDIBits(dc, mask.value, 0, maskHeight, maskPixels.data(), maskInfo.Get(), DIB_RGB_COLORS) != maskHeight) {
        error = WinError(L"Could not write resized cursor masks", GetLastError()); return nullptr;
    }
    ICONINFO result{}; result.fIcon = FALSE;
    result.xHotspot = static_cast<DWORD>(std::clamp(MulDiv(static_cast<int>(hotX), targetWidth, width), 0, targetWidth - 1));
    result.yHotspot = static_cast<DWORD>(std::clamp(MulDiv(static_cast<int>(hotY), targetHeight, height), 0, targetHeight - 1));
    result.hbmColor = color.value; result.hbmMask = mask.value;
    const auto cursor = reinterpret_cast<HCURSOR>(CreateIconIndirect(&result));
    if (!cursor) error = WinError(L"Could not create the resized legacy cursor", GetLastError());
    return cursor;
}

// All ordinary cursor artwork is filtered in premultiplied space, including
// legacy opaque/transparent masks after lossless conversion to alpha, and
// written back with straight alpha. Only genuine background-XOR pixels need
// the discrete two-plane path above.
HCURSOR ResizeCursorArtwork(HCURSOR source, int width, int height, int targetWidth,
                             int targetHeight, DWORD hotX, DWORD hotY,
                             std::wstring& error) {
    IconBitmaps sourceBits;
    if (!GetIconInfo(source, &sourceBits.info)) {
        error = WinError(L"Could not read cursor artwork", GetLastError());
        return nullptr;
    }
    ScreenDC dc;
    if (!dc.value) {
        error = WinError(L"Could not obtain a cursor graphics context", GetLastError());
        return nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    if (sourceBits.info.hbmColor && GetDIBits(dc.value, sourceBits.info.hbmColor, 0, height, pixels.data(),
                  &info, DIB_RGB_COLORS) != height) {
        error = WinError(L"Could not read cursor pixels for resizing", GetLastError());
        return nullptr;
    }
    bool alpha = false;
    for (std::size_t i = 0; i < pixels.size(); i += 4) alpha |= pixels[i + 3] != 0;
    if (!alpha) {
        BITMAP description{};
        const int maskHeight = sourceBits.info.hbmColor ? height : height * 2;
        if (!sourceBits.info.hbmMask || !GetObjectW(sourceBits.info.hbmMask, sizeof(description), &description) ||
            description.bmWidth != width || description.bmHeight != maskHeight) {
            error = L"The cursor has inconsistent legacy transparency dimensions."; return nullptr;
        }
        MonoInfo maskInfo(width, maskHeight);
        const auto stride = MonoStride(width);
        std::vector<std::uint8_t> maskPixels(stride * maskHeight, 0);
        if (GetDIBits(dc.value, sourceBits.info.hbmMask, 0, maskHeight, maskPixels.data(),
                      maskInfo.Get(), DIB_RGB_COLORS) != maskHeight) {
            error = WinError(L"Could not read cursor masks for resizing", GetLastError()); return nullptr;
        }
        bool backgroundXor = false;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            auto* pixel = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            const bool andBit = MaskBit(maskPixels, stride, x, y);
            if (!sourceBits.info.hbmColor)
                pixel[0] = pixel[1] = pixel[2] = MaskBit(maskPixels, stride, x, y + height) ? 255 : 0;
            backgroundXor |= andBit && (pixel[0] || pixel[1] || pixel[2]);
            pixel[3] = andBit ? 0 : 255;
        }
        if (backgroundXor)
            return ResizeXorCursor(sourceBits.info, pixels, maskPixels, width, height,
                                   targetWidth, targetHeight, hotX, hotY, dc.value, error);
    }
    PremultiplyBgra(pixels.data(), pixels.size() / 4);
    info.bmiHeader.biWidth = targetWidth; info.bmiHeader.biHeight = -targetHeight;
    BitmapOwner color, mask;
    void* destination = nullptr;
    color.value = CreateDIBSection(dc.value, &info, DIB_RGB_COLORS, &destination, nullptr, 0);
    mask.value = CreateBitmap(targetWidth, targetHeight, 1, 1, nullptr);
    if (!color.value || !mask.value || !destination) {
        error = WinError(L"Could not allocate resized cursor artwork", GetLastError());
        return nullptr;
    }
    const auto maskStride = MonoStride(targetWidth);
    std::vector<std::uint8_t> maskPixels(maskStride * targetHeight, 0xff);
    auto* output = static_cast<std::uint8_t*>(destination);
    const bool shrinking = targetWidth < width || targetHeight < height;
    for (int y = 0; y < targetHeight; ++y) for (int x = 0; x < targetWidth; ++x) {
        double sum[4]{};
        if (shrinking) {
            const double left = static_cast<double>(x) * width / targetWidth;
            const double right = static_cast<double>(x + 1) * width / targetWidth;
            const double top = static_cast<double>(y) * height / targetHeight;
            const double bottom = static_cast<double>(y + 1) * height / targetHeight;
            const double area = (right - left) * (bottom - top);
            for (int sy = static_cast<int>(top); sy < std::min(height, static_cast<int>(std::ceil(bottom))); ++sy)
                for (int sx = static_cast<int>(left); sx < std::min(width, static_cast<int>(std::ceil(right))); ++sx) {
                    const double weight = (std::min(right, sx + 1.) - std::max(left, static_cast<double>(sx))) *
                                          (std::min(bottom, sy + 1.) - std::max(top, static_cast<double>(sy))) / area;
                    const auto* pixel = pixels.data() + (static_cast<std::size_t>(sy) * width + sx) * 4;
                    for (unsigned channel = 0; channel < 4; ++channel) sum[channel] += pixel[channel] * weight;
                }
        } else {
            const double sx = std::clamp((x + .5) * width / targetWidth - .5, 0., width - 1.);
            const double sy = std::clamp((y + .5) * height / targetHeight - .5, 0., height - 1.);
            const int ix = static_cast<int>(sx), iy = static_cast<int>(sy);
            for (int row = 0; row < 2; ++row) for (int col = 0; col < 2; ++col) {
                const double weight = (col ? sx - ix : 1. - (sx - ix)) * (row ? sy - iy : 1. - (sy - iy));
                const auto* pixel = pixels.data() + (static_cast<std::size_t>(std::min(iy + row, height - 1)) * width +
                                                    std::min(ix + col, width - 1)) * 4;
                for (unsigned channel = 0; channel < 4; ++channel) sum[channel] += pixel[channel] * weight;
            }
        }
        auto* pixel = output + (static_cast<std::size_t>(y) * targetWidth + x) * 4;
        pixel[3] = static_cast<std::uint8_t>(std::clamp(std::lround(sum[3]), 0l, 255l));
        for (unsigned channel = 0; channel < 3; ++channel)
            pixel[channel] = static_cast<std::uint8_t>(std::clamp(std::lround(sum[channel]), 0l, static_cast<long>(pixel[3])));
        if (pixel[3]) maskPixels[static_cast<std::size_t>(y) * maskStride + x / 8] &=
            static_cast<std::uint8_t>(~(0x80u >> (x % 8)));
    }
    UnpremultiplyBgra(output, static_cast<std::size_t>(targetWidth) * targetHeight);
    MonoInfo maskInfo(targetWidth, targetHeight);
    if (SetDIBits(dc.value, mask.value, 0, targetHeight, maskPixels.data(), maskInfo.Get(), DIB_RGB_COLORS) != targetHeight) {
        error = WinError(L"Could not write resized cursor transparency", GetLastError());
        return nullptr;
    }
    ICONINFO cursor{}; cursor.fIcon = FALSE;
    cursor.xHotspot = static_cast<DWORD>(std::clamp(MulDiv(static_cast<int>(hotX), targetWidth, width), 0, targetWidth - 1));
    cursor.yHotspot = static_cast<DWORD>(std::clamp(MulDiv(static_cast<int>(hotY), targetHeight, height), 0, targetHeight - 1));
    cursor.hbmColor = color.value; cursor.hbmMask = mask.value;
    const HCURSOR result = reinterpret_cast<HCURSOR>(CreateIconIndirect(&cursor));
    if (!result) error = WinError(L"Could not create resized cursor artwork", GetLastError());
    return result;
}

HCURSOR CopyAtSize(HCURSOR source, int size, std::wstring& error) {
    if (!size) {
        HCURSOR result = CopyOwnedCursor(source);
        if (!result) error = WinError(L"Could not copy cursor artwork", GetLastError());
        return result;
    }
    int width = 0, height = 0; DWORD hotX = 0, hotY = 0;
    if (!CursorDimensions(source, width, height, hotX, hotY, error)) return nullptr;
    const int longest = std::max(width, height);
    const int targetWidth = std::max(1, MulDiv(width, size, longest));
    const int targetHeight = std::max(1, MulDiv(height, size, longest));
    if (targetWidth == width && targetHeight == height) {
        const HCURSOR result = CopyOwnedCursor(source);
        if (!result) error = WinError(L"Could not copy cursor artwork", GetLastError());
        return result;
    }
    return ResizeCursorArtwork(source, width, height, targetWidth, targetHeight, hotX, hotY, error);
}

bool LookActive(const CursorAppearance& look) noexcept {
    return look.opacity < 100 || look.shadow || look.tint;
}

// Premultiplied BGRA pixels of a cursor (Windows stores straight alpha),
// legacy masks converted losslessly.
// Returns false with an empty error for genuine background-XOR artwork, which
// alpha cannot represent; such a cursor keeps its look.
bool ReadPremultiplied(HCURSOR cursor, std::vector<std::uint8_t>& pixels, int& width, int& height,
                       DWORD& hotX, DWORD& hotY, std::wstring& error) {
    if (!CursorDimensions(cursor, width, height, hotX, hotY, error)) return false;
    IconBitmaps bits;
    if (!GetIconInfo(cursor, &bits.info)) { error = WinError(L"Could not read cursor artwork", GetLastError()); return false; }
    ScreenDC dc;
    if (!dc.value) { error = WinError(L"Could not obtain a cursor graphics context", GetLastError()); return false; }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);
    if (bits.info.hbmColor && GetDIBits(dc.value, bits.info.hbmColor, 0, height, pixels.data(), &info, DIB_RGB_COLORS) != height) {
        error = WinError(L"Could not read cursor pixels", GetLastError()); return false;
    }
    bool alpha = false;
    for (std::size_t i = 0; i < pixels.size(); i += 4) alpha |= pixels[i + 3] != 0;
    if (!alpha) {
        BITMAP description{};
        const int maskHeight = bits.info.hbmColor ? height : height * 2;
        if (!bits.info.hbmMask || !GetObjectW(bits.info.hbmMask, sizeof(description), &description) ||
            description.bmWidth != width || description.bmHeight != maskHeight) {
            error = L"The cursor has inconsistent legacy transparency dimensions."; return false;
        }
        MonoInfo maskInfo(width, maskHeight);
        const auto stride = MonoStride(width);
        std::vector<std::uint8_t> mask(stride * maskHeight, 0);
        if (GetDIBits(dc.value, bits.info.hbmMask, 0, maskHeight, mask.data(), maskInfo.Get(), DIB_RGB_COLORS) != maskHeight) {
            error = WinError(L"Could not read cursor masks", GetLastError()); return false;
        }
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            auto* pixel = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            const bool andBit = MaskBit(mask, stride, x, y);
            if (!bits.info.hbmColor) pixel[0] = pixel[1] = pixel[2] = MaskBit(mask, stride, x, y + height) ? 255 : 0;
            if (andBit && (pixel[0] || pixel[1] || pixel[2])) { pixels.clear(); return false; }
            pixel[3] = andBit ? 0 : 255;
        }
    }
    PremultiplyBgra(pixels.data(), pixels.size() / 4);
    return true;
}

HCURSOR CursorFromPremultiplied(const std::vector<std::uint8_t>& pixels, int width, int height,
                                DWORD hotX, DWORD hotY, std::wstring& error) {
    ScreenDC dc;
    if (!dc.value) { error = WinError(L"Could not obtain a cursor graphics context", GetLastError()); return nullptr; }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    BitmapOwner color, mask;
    void* destination = nullptr;
    color.value = CreateDIBSection(dc.value, &info, DIB_RGB_COLORS, &destination, nullptr, 0);
    mask.value = CreateBitmap(width, height, 1, 1, nullptr);
    if (!color.value || !mask.value || !destination) {
        error = WinError(L"Could not allocate cursor artwork", GetLastError()); return nullptr;
    }
    CopyMemory(destination, pixels.data(), pixels.size());
    UnpremultiplyBgra(static_cast<std::uint8_t*>(destination), pixels.size() / 4);
    const auto stride = MonoStride(width);
    std::vector<std::uint8_t> maskPixels(stride * height, 0xff);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        if (pixels[(static_cast<std::size_t>(y) * width + x) * 4 + 3])
            maskPixels[static_cast<std::size_t>(y) * stride + x / 8] &= static_cast<std::uint8_t>(~(0x80u >> (x % 8)));
    MonoInfo maskInfo(width, height);
    if (SetDIBits(dc.value, mask.value, 0, height, maskPixels.data(), maskInfo.Get(), DIB_RGB_COLORS) != height) {
        error = WinError(L"Could not write cursor transparency", GetLastError()); return nullptr;
    }
    ICONINFO cursor{}; cursor.fIcon = FALSE;
    cursor.xHotspot = hotX; cursor.yHotspot = hotY;
    cursor.hbmColor = color.value; cursor.hbmMask = mask.value;
    const HCURSOR result = reinterpret_cast<HCURSOR>(CreateIconIndirect(&cursor));
    if (!result) error = WinError(L"Could not create cursor artwork", GetLastError());
    return result;
}

// Recolors, shadows and fades premultiplied artwork in place. The shadow
// grows the canvas to the right and bottom (and by its blur on the other
// sides, moving the hotspot with the artwork); it shrinks to stay within
// Windows' 256-pixel cursor limit.
void LookPixels(std::vector<std::uint8_t>& pixels, int& width, int& height, DWORD& hotX, DWORD& hotY,
                const CursorAppearance& look) {
    if (look.tint) {
        const double fill[3]{double(GetBValue(look.fillColor)), double(GetGValue(look.fillColor)), double(GetRValue(look.fillColor))};
        const double line[3]{double(GetBValue(look.outlineColor)), double(GetGValue(look.outlineColor)), double(GetRValue(look.outlineColor))};
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            const unsigned a = pixels[i + 3];
            if (!a) continue;
            const double b = pixels[i] * 255.0 / a, g = pixels[i + 1] * 255.0 / a, r = pixels[i + 2] * 255.0 / a;
            const double light = std::clamp((0.114 * b + 0.587 * g + 0.299 * r) / 255.0, 0.0, 1.0);
            for (unsigned channel = 0; channel < 3; ++channel) {
                const double straight = line[channel] + (fill[channel] - line[channel]) * light;
                pixels[i + channel] = static_cast<std::uint8_t>(std::clamp(std::lround(straight * a / 255.0), 0l, static_cast<long>(a)));
            }
        }
    }
    if (look.shadow) {
        const int longest = std::max(width, height);
        int offset = std::max(1, static_cast<int>(std::lround(longest / 16.0)));
        int blur = std::max(1, static_cast<int>(std::lround(longest / 16.0)));
        while ((width + offset + 2 * blur > 256 || height + offset + 2 * blur > 256) && (offset > 0 || blur > 0)) {
            if (blur > 0) --blur; else --offset;
        }
        if (offset > 0 || blur > 0) {
            const int padLeft = blur, padTop = blur;
            const int nextWidth = width + offset + 2 * blur, nextHeight = height + offset + 2 * blur;
            std::vector<float> shade(static_cast<std::size_t>(nextWidth) * nextHeight, 0.f), scratch(shade.size(), 0.f);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                shade[static_cast<std::size_t>(y + padTop + offset) * nextWidth + x + padLeft + offset] =
                    pixels[(static_cast<std::size_t>(y) * width + x) * 4 + 3] / 255.f;
            // Two box passes approximate a gaussian; each pass blurs rows, then columns.
            for (int pass = 0; pass < 2 && blur > 0; ++pass) {
                const float scale = 1.f / (2 * blur + 1);
                for (int y = 0; y < nextHeight; ++y) for (int x = 0; x < nextWidth; ++x) {
                    float sum = 0.f;
                    for (int k = -blur; k <= blur; ++k) {
                        const int sx = x + k;
                        if (sx >= 0 && sx < nextWidth) sum += shade[static_cast<std::size_t>(y) * nextWidth + sx];
                    }
                    scratch[static_cast<std::size_t>(y) * nextWidth + x] = sum * scale;
                }
                for (int y = 0; y < nextHeight; ++y) for (int x = 0; x < nextWidth; ++x) {
                    float sum = 0.f;
                    for (int k = -blur; k <= blur; ++k) {
                        const int sy = y + k;
                        if (sy >= 0 && sy < nextHeight) sum += scratch[static_cast<std::size_t>(sy) * nextWidth + x];
                    }
                    shade[static_cast<std::size_t>(y) * nextWidth + x] = sum * scale;
                }
            }
            std::vector<std::uint8_t> output(static_cast<std::size_t>(nextWidth) * nextHeight * 4, 0);
            for (int y = 0; y < nextHeight; ++y) for (int x = 0; x < nextWidth; ++x) {
                auto* target = output.data() + (static_cast<std::size_t>(y) * nextWidth + x) * 4;
                const int sx = x - padLeft, sy = y - padTop;
                const std::uint8_t* art = sx >= 0 && sy >= 0 && sx < width && sy < height
                    ? pixels.data() + (static_cast<std::size_t>(sy) * width + sx) * 4 : nullptr;
                const double artAlpha = art ? art[3] / 255.0 : 0.0;
                const double shadowAlpha = 0.38 * shade[static_cast<std::size_t>(y) * nextWidth + x] * (1.0 - artAlpha);
                for (unsigned channel = 0; channel < 3; ++channel) target[channel] = art ? art[channel] : 0;
                target[3] = static_cast<std::uint8_t>(std::clamp(std::lround((artAlpha + shadowAlpha) * 255.0), 0l, 255l));
            }
            pixels = std::move(output);
            width = nextWidth; height = nextHeight;
            hotX += static_cast<DWORD>(padLeft); hotY += static_cast<DWORD>(padTop);
        }
    }
    if (look.opacity < 100) {
        const unsigned opacity = static_cast<unsigned>(std::clamp(look.opacity, 0, 100));
        for (auto& value : pixels) value = static_cast<std::uint8_t>((value * opacity + 50u) / 100u);
    }
}

// Replaces `cursor` with its look applied. Artwork alpha cannot represent
// (background-inverting legacy cursors) is left unchanged.
bool ApplyLook(HCURSOR& cursor, const CursorAppearance& look, std::wstring& error) {
    if (!cursor || !LookActive(look)) return true;
    std::vector<std::uint8_t> pixels;
    int width = 0, height = 0; DWORD hotX = 0, hotY = 0;
    std::wstring readError;
    if (!ReadPremultiplied(cursor, pixels, width, height, hotX, hotY, readError)) {
        if (readError.empty()) return true;
        error = readError; return false;
    }
    LookPixels(pixels, width, height, hotX, hotY, look);
    const HCURSOR next = CursorFromPremultiplied(pixels, width, height, hotX, hotY, error);
    if (!next) return false;
    DestroyCursor(cursor);
    cursor = next;
    return true;
}

std::uint16_t Read16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}

std::uint32_t Read32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

struct CursorFrame {
    unsigned width = 0, height = 0, hotX = 0, hotY = 0, depth = 0;
    std::uint32_t bytes = 0, offset = 0;
};

// Validates a .cur image directory held in memory (a file, or one frame of an
// animated cursor) and lists its frames.
bool ParseCursorDirectory(const std::vector<std::uint8_t>& bytes, std::vector<CursorFrame>& frames,
                          std::wstring& error) {
    if (bytes.size() < 22) { error = L"This is not a valid static .cur image directory."; return false; }
    const unsigned count = Read16(bytes.data() + 4);
    if (Read16(bytes.data()) || Read16(bytes.data() + 2) != 2 || !count || count > 256 ||
        bytes.size() < 6 + count * 16) {
        error = L"This is not a valid static .cur image directory.";
        return false;
    }
    frames.clear();
    frames.reserve(count);
    for (unsigned n = 0; n < count; ++n) {
        const auto* p = bytes.data() + 6 + n * 16;
        CursorFrame frame;
        frame.width = p[0] ? p[0] : 256; frame.height = p[1] ? p[1] : 256;
        frame.hotX = Read16(p + 4); frame.hotY = Read16(p + 6);
        frame.bytes = Read32(p + 8); frame.offset = Read32(p + 12);
        if (frame.hotX >= frame.width || frame.hotY >= frame.height || frame.bytes < 12 ||
            frame.offset < 6 + count * 16 || frame.offset > bytes.size() ||
            frame.bytes > bytes.size() - frame.offset) {
            error = L"The cursor file contains invalid image bounds or a hotspot outside its artwork.";
            return false;
        }
        const auto* image = bytes.data() + frame.offset;
        constexpr std::array<std::uint8_t, 8> png{{137,80,78,71,13,10,26,10}};
        unsigned width = 0, height = 0;
        if (frame.bytes >= 29 && std::equal(png.begin(), png.end(), image)) {
            const auto big32 = [](const std::uint8_t* data) {
                return static_cast<std::uint32_t>(data[3]) | (static_cast<std::uint32_t>(data[2]) << 8) |
                       (static_cast<std::uint32_t>(data[1]) << 16) | (static_cast<std::uint32_t>(data[0]) << 24);
            };
            if (big32(image + 8) != 13 || Read32(image + 12) != 0x52444849u) {
                error = L"The cursor contains an invalid PNG image header."; return false;
            }
            width = big32(image + 16); height = big32(image + 20);
            frame.depth = image[24] * (image[25] == 6 ? 4u : image[25] == 2 ? 3u : image[25] == 4 ? 2u : 1u);
        } else if (Read32(image) == 12) {
            width = Read16(image + 4); height = Read16(image + 6);
            if ((height & 1u) || Read16(image + 8) != 1) height = 0;
            else height /= 2;
            frame.depth = Read16(image + 10);
        } else if (Read32(image) >= 40 && Read32(image) <= frame.bytes && frame.bytes >= 40) {
            width = Read32(image + 4); height = Read32(image + 8);
            if (height & 0x80000000u) height = (~height) + 1;
            if ((height & 1u) || Read16(image + 12) != 1) height = 0;
            else height /= 2;
            frame.depth = Read16(image + 14);
        }
        if (width != frame.width || height != frame.height || !frame.depth || frame.depth > 64) {
            error = L"The cursor image dimensions or color format do not match its directory.";
            return false;
        }
        frames.push_back(frame);
    }
    return true;
}

bool ReadStaticFile(const std::wstring& path, std::vector<std::uint8_t>& bytes,
                    std::vector<CursorFrame>& frames, std::wstring& error) {
    std::wstring extension = path.size() >= 4 ? path.substr(path.size() - 4) : L"";
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (extension != L".cur") {
        error = L"Choose a static .cur file. Animated .ani cursors are not supported in an arrow pair.";
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = WinError(L"Could not open the cursor file", GetLastError());
        return false;
    }
    LARGE_INTEGER length{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &length) ||
        length.QuadPart < 22 || length.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        error = L"This is not a valid static .cur file (maximum file size: 16 MiB).";
        return false;
    }
    try { bytes.resize(static_cast<std::size_t>(length.QuadPart)); }
    catch (...) { CloseHandle(file); throw; }
    DWORD read = 0;
    const bool complete = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                          read == bytes.size();
    CloseHandle(file);
    if (!complete) { error = L"The cursor file could not be read completely."; return false; }
    return ParseCursorDirectory(bytes, frames, error);
}

HCURSOR DecodeStaticFile(const std::vector<std::uint8_t>& bytes,
                         const std::vector<CursorFrame>& frames, int size,
                         std::wstring& error) {
    const unsigned wanted = size ? static_cast<unsigned>(size) : std::max(frames.front().width, frames.front().height);
    const auto rank = [wanted](const CursorFrame& frame) {
        const unsigned extent = std::max(frame.width, frame.height);
        // Prefer an exact frame, otherwise downsample the next larger one.
        // Only upscale when the file has no image at least as large as wanted.
        return std::make_pair(extent >= wanted ? 0u : 1u,
                              extent >= wanted ? extent - wanted : wanted - extent);
    };
    const CursorFrame* selected = &frames.front();
    for (const auto& frame : frames) {
        if (!size && (frame.width != frames.front().width || frame.height != frames.front().height)) continue;
        if (rank(frame) < rank(*selected) || (rank(frame) == rank(*selected) && frame.depth > selected->depth))
            selected = &frame;
    }
    // CUR files keep hotspots in the directory; RT_CURSOR resource data puts
    // the two WORDs first. Decode that exact frame through Windows, including
    // PNG-compressed frames, without its zero-size first-image selection.
    const std::size_t resourceBytes = static_cast<std::size_t>(selected->bytes) + 4;
    std::vector<std::uint32_t> aligned((resourceBytes + 3) / 4, 0);
    aligned[0] = selected->hotX | (selected->hotY << 16);
    CopyMemory(reinterpret_cast<std::uint8_t*>(aligned.data()) + 4,
               bytes.data() + selected->offset, selected->bytes);
    CursorOwner source;
    source.value = reinterpret_cast<HCURSOR>(CreateIconFromResourceEx(
        reinterpret_cast<PBYTE>(aligned.data()), static_cast<DWORD>(resourceBytes),
        FALSE, 0x00030000, 0, 0, 0));
    if (!source.value) { error = WinError(L"Windows could not decode this cursor image", GetLastError()); return nullptr; }
    int width = 0, height = 0; DWORD hotX = 0, hotY = 0;
    if (!CursorDimensions(source.value, width, height, hotX, hotY, error)) return nullptr;
    if (width != static_cast<int>(selected->width) || height != static_cast<int>(selected->height) ||
        hotX != selected->hotX || hotY != selected->hotY) {
        error = L"Windows decoded different cursor dimensions or a different hotspot.";
        return nullptr;
    }
    return CopyAtSize(source.value, size, error);
}

// One frame as .cur file data: a single 32-bit image with its AND mask.
// Cursor files store straight alpha, like every Windows cursor bitmap.
bool EncodeCursorFile(HCURSOR cursor, std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> pixels;
    int width = 0, height = 0; DWORD hotX = 0, hotY = 0;
    std::wstring ignored;
    if (!ReadPremultiplied(cursor, pixels, width, height, hotX, hotY, ignored) || width > 256 || height > 256) return false;
    const auto maskStride = static_cast<std::size_t>((width + 31) / 32 * 4);
    const auto colorBytes = static_cast<std::size_t>(width) * height * 4;
    const auto imageBytes = 40 + colorBytes + maskStride * height;
    out.assign(22 + imageBytes, 0);
    const auto put16 = [&](std::size_t at, unsigned value) { out[at] = static_cast<std::uint8_t>(value); out[at + 1] = static_cast<std::uint8_t>(value >> 8); };
    const auto put32 = [&](std::size_t at, std::uint32_t value) { for (int i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (8 * i)); };
    put16(2, 2); put16(4, 1);
    out[6] = static_cast<std::uint8_t>(width & 0xff); out[7] = static_cast<std::uint8_t>(height & 0xff);
    put16(10, hotX); put16(12, hotY);
    put32(14, static_cast<std::uint32_t>(imageBytes)); put32(18, 22);
    put32(22, 40); put32(26, static_cast<std::uint32_t>(width)); put32(30, static_cast<std::uint32_t>(height * 2));
    put16(34, 1); put16(36, 32); put32(42, static_cast<std::uint32_t>(colorBytes + maskStride * height));
    for (int y = 0; y < height; ++y) {
        const auto* row = pixels.data() + static_cast<std::size_t>(height - 1 - y) * width * 4;
        auto* color = out.data() + 62 + static_cast<std::size_t>(y) * width * 4;
        auto* mask = out.data() + 62 + colorBytes + static_cast<std::size_t>(y) * maskStride;
        for (int x = 0; x < width; ++x) {
            const unsigned a = row[x * 4 + 3];
            for (unsigned channel = 0; channel < 3; ++channel)
                color[x * 4 + channel] = a ? static_cast<std::uint8_t>(std::min(255u, (row[x * 4 + channel] * 255u + a / 2) / a)) : 0;
            color[x * 4 + 3] = static_cast<std::uint8_t>(a);
            if (!a) mask[x / 8] |= static_cast<std::uint8_t>(0x80u >> (x % 8));
        }
    }
    return true;
}

// Rebuilds an animated .ani cursor with every frame decoded at `size` and
// given the cursor look, keeping its frame order and timing. Returns empty
// data when the file cannot be processed; the role then keeps its plain
// animation instead.
std::vector<std::uint8_t> AnimationWithLook(const std::wstring& path, int size, const CursorAppearance& look) {
    std::vector<std::uint8_t> file;
    {
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER length{};
        if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileSizeEx(handle, &length) || length.QuadPart < 12 ||
            length.QuadPart > 16 * 1024 * 1024) { CloseHandle(handle); return {}; }
        file.resize(static_cast<std::size_t>(length.QuadPart));
        DWORD read = 0;
        const bool complete = ReadFile(handle, file.data(), static_cast<DWORD>(file.size()), &read, nullptr) && read == file.size();
        CloseHandle(handle);
        if (!complete) return {};
    }
    const auto tag = [&](std::size_t at, const char* name) { return at + 4 <= file.size() && std::memcmp(file.data() + at, name, 4) == 0; };
    if (!tag(0, "RIFF") || !tag(8, "ACON")) return {};
    std::vector<std::uint8_t> header, rate, sequence;
    std::vector<std::vector<std::uint8_t>> frames;
    const std::size_t end = std::min<std::size_t>(file.size(), 8 + static_cast<std::size_t>(Read32(file.data() + 4)));
    for (std::size_t at = 12; at + 8 <= end;) {
        const std::size_t length = Read32(file.data() + at + 4);
        const std::size_t data = at + 8;
        if (length > end - data) return {};
        const auto chunk = [&] { return std::vector<std::uint8_t>(file.begin() + static_cast<std::ptrdiff_t>(data), file.begin() + static_cast<std::ptrdiff_t>(data + length)); };
        if (tag(at, "anih")) header = chunk();
        else if (tag(at, "rate")) rate = chunk();
        else if (tag(at, "seq ")) sequence = chunk();
        else if (tag(at, "LIST") && length >= 4 && tag(data, "fram")) {
            for (std::size_t inner = data + 4; inner + 8 <= data + length;) {
                const std::size_t innerLength = Read32(file.data() + inner + 4);
                if (innerLength > data + length - (inner + 8)) return {};
                if (tag(inner, "icon"))
                    frames.emplace_back(file.begin() + static_cast<std::ptrdiff_t>(inner + 8),
                                        file.begin() + static_cast<std::ptrdiff_t>(inner + 8 + innerLength));
                inner += 8 + innerLength + (innerLength & 1);
            }
        }
        at = data + length + (length & 1);
    }
    if (header.size() < 36 || frames.empty() || frames.size() > 1024) return {};
    std::vector<std::vector<std::uint8_t>> styled;
    styled.reserve(frames.size());
    for (const auto& frame : frames) {
        std::vector<CursorFrame> entries;
        std::wstring ignored;
        if (!ParseCursorDirectory(frame, entries, ignored)) return {};
        CursorOwner decoded;
        decoded.value = DecodeStaticFile(frame, entries, size, ignored);
        if (!decoded.value || !ApplyLook(decoded.value, look, ignored)) return {};
        std::vector<std::uint8_t> encoded;
        if (!EncodeCursorFile(decoded.value, encoded)) return {};
        styled.push_back(std::move(encoded));
    }
    // anih: frame count at offset 4; frames are icon chunks (flag bit 0).
    const auto set32 = [](std::vector<std::uint8_t>& data, std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) data[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    set32(header, 4, static_cast<std::uint32_t>(styled.size()));
    set32(header, 32, Read32(header.data() + 32) | 1u);
    std::vector<std::uint8_t> list{'f', 'r', 'a', 'm'};
    const auto append = [](std::vector<std::uint8_t>& target, const char* name, const std::vector<std::uint8_t>& data) {
        target.insert(target.end(), name, name + 4);
        for (int i = 0; i < 4; ++i) target.push_back(static_cast<std::uint8_t>(data.size() >> (8 * i)));
        target.insert(target.end(), data.begin(), data.end());
        if (data.size() & 1) target.push_back(0);
    };
    for (const auto& frame : styled) append(list, "icon", frame);
    std::vector<std::uint8_t> body{'A', 'C', 'O', 'N'};
    append(body, "anih", header);
    if (!rate.empty()) append(body, "rate", rate);
    if (!sequence.empty()) append(body, "seq ", sequence);
    append(body, "LIST", list);
    std::vector<std::uint8_t> output;
    append(output, "RIFF", body);
    return output;
}

HCURSOR LoadStaticFile(const std::wstring& path, int size, std::wstring& error) {
    std::vector<std::uint8_t> bytes;
    std::vector<CursorFrame> frames;
    if (!ReadStaticFile(path, bytes, frames, error)) return nullptr;
    return DecodeStaticFile(bytes, frames, size, error);
}

bool SameCursorArtwork(HCURSOR first, HCURSOR second) {
    // A registered file is only a resolution source when its native frame is
    // the captured artwork. Windows accessibility colors and other live
    // replacements can differ from that file; keep the saved image then.
    IconBitmaps a, b;
    if (!GetIconInfo(first, &a.info) || !GetIconInfo(second, &b.info) ||
        a.info.xHotspot != b.info.xHotspot || a.info.yHotspot != b.info.yHotspot ||
        !!a.info.hbmColor != !!b.info.hbmColor) return false;
    ScreenDC dc;
    if (!dc.value) return false;
    const auto sameBitmap = [&](HBITMAP x, HBITMAP y) {
        BITMAP dx{}, dy{};
        if (!x || !y || !GetObjectW(x, sizeof(dx), &dx) ||
            !GetObjectW(y, sizeof(dy), &dy) || !ValidSize(dx) || !ValidSize(dy) ||
            dx.bmWidth != dy.bmWidth || dx.bmHeight != dy.bmHeight) return false;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = dx.bmWidth; info.bmiHeader.biHeight = -dx.bmHeight;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint8_t> px(static_cast<std::size_t>(dx.bmWidth) * dx.bmHeight * 4), py(px.size());
        return GetDIBits(dc.value, x, 0, dx.bmHeight, px.data(), &info, DIB_RGB_COLORS) == dx.bmHeight &&
               GetDIBits(dc.value, y, 0, dy.bmHeight, py.data(), &info, DIB_RGB_COLORS) == dy.bmHeight && px == py;
    };
    return sameBitmap(a.info.hbmMask, b.info.hbmMask) &&
           (!a.info.hbmColor || sameBitmap(a.info.hbmColor, b.info.hbmColor));
}

struct StaticArtwork {
    std::vector<std::uint8_t> bytes;
    std::vector<CursorFrame> frames;
    bool Read(const std::wstring& path, std::wstring& error) {
        bytes.clear(); frames.clear();
        return ReadStaticFile(path, bytes, frames, error);
    }
    HCURSOR Make(int size, std::wstring& error) const {
        return DecodeStaticFile(bytes, frames, size, error);
    }
    bool ReadMatching(const std::wstring& path, HCURSOR original) {
        if (path.empty()) return false;
        std::wstring ignored;
        if (!Read(path, ignored)) return false;
        int width = 0, height = 0; DWORD x = 0, y = 0;
        if (!CursorDimensions(original, width, height, x, y, ignored)) return false;
        CursorOwner candidate; candidate.value = Make(std::max(width, height), ignored);
        return candidate.value && SameCursorArtwork(original, candidate.value);
    }
};

HCURSOR CreateClassicArrow(int size, std::wstring& error) {
    // Manual sizes and installations without the local classic file use the
    // same canonical silhouette sampled at final resolution. Enlarging the
    // local monochrome bitmap would retain its coarse diagonal pixel steps.
    // The outline matches the classic Windows arrow's proportions inside its
    // 32-unit canvas (about 13 wide and 20 tall, tip at the hotspot), so an
    // explicit size looks like the original arrow_r.cur scaled, and sits in
    // proportion to every other resized cursor shape.
    constexpr std::array<std::array<double, 2>, 7> polygon{{
        {{0.5, 0.5}}, {{0.5, 17.4}}, {{4.9, 13.2}}, {{8.1, 20.0}}, {{10.9, 18.8}}, {{7.8, 12.1}}, {{13.4, 12.1}}
    }};
    size = std::clamp(size, 1, 4096);
    const auto inside = [&](double x, double y) {
        bool result = false;
        for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
            const auto& a = polygon[i]; const auto& b = polygon[j];
            if ((a[1] > y) != (b[1] > y) &&
                x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) result = !result;
        }
        return result;
    };
    ScreenDC dc;
    if (!dc.value) { error = L"Could not obtain a cursor graphics context."; return nullptr; }
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size; info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* data = nullptr; BitmapOwner color, mask;
    color.value = CreateDIBSection(dc.value, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    mask.value = CreateBitmap(size, size, 1, 1, nullptr);
    if (!color.value || !data || !mask.value) { error = L"Could not allocate classic-style cursor artwork."; return nullptr; }
    const auto stride = MonoStride(size); std::vector<std::uint8_t> transparency(stride * size, 0xff);
    auto* pixels = static_cast<std::uint8_t*>(data);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        auto* p = pixels + (static_cast<std::size_t>(y) * size + x) * 4;
        p[0] = p[1] = p[2] = p[3] = 0;
        unsigned covered = 0, fill = 0;
        constexpr unsigned samples = 4;
        for (unsigned sy = 0; sy < samples; ++sy) for (unsigned sx = 0; sx < samples; ++sx) {
            const double px = (x + (sx + .5) / samples) * 32.0 / size;
            const double py = (y + (sy + .5) / samples) * 32.0 / size;
            if (!inside(px, py)) continue;
            ++covered;
            // Distance to every edge gives a consistent one-reference-pixel
            // outline, including the two diagonals and the narrow tail.
            double nearest = std::numeric_limits<double>::max();
            for (std::size_t a = 0, b = polygon.size() - 1; a < polygon.size(); b = a++) {
                const double dx = polygon[a][0] - polygon[b][0], dy = polygon[a][1] - polygon[b][1];
                const double t = std::clamp(((px - polygon[b][0]) * dx + (py - polygon[b][1]) * dy) /
                                            (dx * dx + dy * dy), 0., 1.);
                const double ex = px - polygon[b][0] - t * dx, ey = py - polygon[b][1] - t * dy;
                nearest = std::min(nearest, ex * ex + ey * ey);
            }
            if (nearest >= 1.) ++fill;
        }
        if (!covered) continue;
        p[0] = p[1] = p[2] = static_cast<std::uint8_t>((fill * 255u + covered / 2) / covered);
        p[3] = static_cast<std::uint8_t>((covered * 255u + 8u) / 16u);
        transparency[static_cast<std::size_t>(y) * stride + x / 8] &=
            static_cast<std::uint8_t>(~(0x80u >> (x % 8)));
    }
    MonoInfo maskInfo(size, size);
    if (SetDIBits(dc.value, mask.value, 0, size, transparency.data(), maskInfo.Get(), DIB_RGB_COLORS) != size) {
        error = L"Could not create classic-style cursor transparency."; return nullptr;
    }
    ICONINFO icon{}; icon.fIcon = FALSE;
    icon.xHotspot = icon.yHotspot = 0; // The tip, as in the classic Windows file.
    icon.hbmColor = color.value; icon.hbmMask = mask.value;
    HCURSOR result = reinterpret_cast<HCURSOR>(CreateIconIndirect(&icon));
    if (!result) error = WinError(L"Could not create the classic-style arrow", GetLastError());
    return result;
}

std::wstring LocalWindowsCursor(const wchar_t* filename) {
    std::array<wchar_t, 32768> windows{};
    const UINT length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (!length || length >= windows.size()) return {};
    return std::wstring(windows.data(), length) + L"\\Cursors\\" + filename;
}

HCURSOR LoadClassicArrow(int size, bool originalSize, std::wstring& error) {
    // Windows Black's normal-select file provides the native classic outline.
    // Derive its light appearance from the same artwork; do not substitute this
    // legacy silhouette for the modern Windows arrow preset. Explicit sizes
    // use final-resolution Classic geometry below instead of stretching it.
    const auto path = originalSize ? LocalWindowsCursor(L"arrow_r.cur") : std::wstring{};
    if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CursorOwner dark;
        dark.value = LoadStaticFile(path, size, error);
        if (dark.value) {
            const HCURSOR light = CreateInvertedCursor(dark.value, error);
            if (light) return light;
        }
        // A missing/corrupt optional OS asset must not strand the classic mode.
        error.clear();
    }
    return CreateClassicArrow(size, error);
}

struct CursorSlot {
    DWORD id = 0;
    HCURSOR shared = nullptr;
    HCURSOR previousShared = nullptr;
    HCURSOR original = nullptr;
    HCURSOR released = nullptr;
    HCURSOR inverted = nullptr;
    HCURSOR applied = nullptr; // Non-owning: one of the prepared master images.
    std::wstring sourcePath;
    bool touched = false;
    bool pressedApplied = false;
    bool baselineChanged = false;
    // An animated role (.ani or a built-in animation). Its "original" is only
    // an identity token: installation reloads the file at animatedSize, the
    // press/shake effects leave it alone, and restoration reloads the scheme.
    bool animated = false;
    int animatedSize = 0;
    // An animation rebuilt with the cursor look; installation creates the
    // cursor from these .ani bytes instead of reloading the file.
    std::vector<std::uint8_t> animation;

    CursorSlot() = default;
    CursorSlot(const CursorSlot&) = delete;
    CursorSlot& operator=(const CursorSlot&) = delete;
    CursorSlot(CursorSlot&& other) noexcept
        : id(other.id), shared(other.shared), previousShared(other.previousShared),
          original(std::exchange(other.original, nullptr)),
          released(std::exchange(other.released, nullptr)),
          inverted(std::exchange(other.inverted, nullptr)), applied(other.applied),
          sourcePath(std::move(other.sourcePath)), touched(other.touched),
          pressedApplied(other.pressedApplied), baselineChanged(other.baselineChanged),
          animated(other.animated), animatedSize(other.animatedSize), animation(std::move(other.animation)) {}
    CursorSlot& operator=(CursorSlot&&) = delete;
    ~CursorSlot() {
        if (original) DestroyCursor(original);
        if (released) DestroyCursor(released);
        if (inverted) DestroyCursor(inverted);
    }
};

bool CaptureOriginals(std::vector<CursorSlot>& slots, std::wstring& error) {
    slots.reserve(std::size(kRoles));
    for (const Role& role : kRoles) {
        auto path = RegisteredCursorPath(role.registryName);
        auto lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        CursorSlot slot;
        slot.id = role.id;
        const bool aniFile = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".ani") == 0;
        const bool curFile = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".cur") == 0;
        if (aniFile || curFile) slot.sourcePath = std::move(path);
        // Wait/AppStarting without a registered file are built-in animations.
        slot.animated = aniFile || (slot.sourcePath.empty() && (role.id == kWaitCursor || role.id == kAppStartingCursor));
        slot.shared = LoadCursorW(nullptr, MAKEINTRESOURCEW(role.id));
        if (!slot.shared) continue;
        slot.original = CopyOwnedCursor(slot.shared);
        if (!slot.original) {
            error = WinError(L"Could not save an original system cursor", GetLastError());
            return false;
        }
        slots.push_back(std::move(slot));
    }
    if (slots.empty()) {
        error = L"This cursor scheme has no supported static system cursors.";
        return false;
    }
    return true;
}

struct MagnifiedArrow {
    float factor = 1.f;
    HCURSOR released = nullptr;
    HCURSOR pressed = nullptr;
    MagnifiedArrow() = default;
    MagnifiedArrow(const MagnifiedArrow&) = delete;
    MagnifiedArrow& operator=(const MagnifiedArrow&) = delete;
    MagnifiedArrow(MagnifiedArrow&& other) noexcept
        : factor(other.factor), released(std::exchange(other.released, nullptr)),
          pressed(std::exchange(other.pressed, nullptr)) {}
    MagnifiedArrow& operator=(MagnifiedArrow&&) = delete;
    ~MagnifiedArrow() {
        if (released) DestroyCursor(released);
        if (pressed) DestroyCursor(pressed);
    }
};

template<class MakeReleased, class MakePressed>
bool BuildMagnification(std::vector<MagnifiedArrow>& output, const CursorSlot& arrow,
                        const CursorAppearance& appearance, MakeReleased makeReleased,
                        MakePressed makePressed, std::wstring& error) {
    if (appearance.maximumMagnification <= 1.f) return true;
    int rw = 0, rh = 0, pw = 0, ph = 0; DWORD x = 0, y = 0;
    // The builders below return the unswapped pair, so measure that same pair.
    const HCURSOR normal = appearance.reverse ? arrow.inverted : arrow.released;
    const HCURSOR down = appearance.reverse ? arrow.released : arrow.inverted;
    if (!CursorDimensions(normal, rw, rh, x, y, error) ||
        !CursorDimensions(down, pw, ph, x, y, error)) return false;
    const int releasedSize = std::max(rw, rh), pressedSize = std::max(pw, ph);
    // Do not shrink an already larger accessibility cursor just to fit the
    // optional animation budget. Such a source keeps its normal appearance.
    if (releasedSize >= 256 || pressedSize >= 256) return true;
    struct Candidate { float factor; int releasedSize, pressedSize; std::size_t bytes; };
    std::vector<Candidate> candidates;
    int previousReleased = releasedSize, previousPressed = pressedSize;
    const auto pixelBytes = [](int width, int height, int size) {
        const int longest = std::max(width, height);
        const int w = std::max(1, MulDiv(width, size, longest));
        const int h = std::max(1, MulDiv(height, size, longest));
        return static_cast<std::size_t>(w) * h * 4 + MonoStride(w) * h;
    };
    for (unsigned step = 1; step <= 48; ++step) {
        const float factor = std::min(1.f + step / 16.f, appearance.maximumMagnification);
        const int r = std::min(256, static_cast<int>(std::lround(releasedSize * factor)));
        const int p = std::min(256, static_cast<int>(std::lround(pressedSize * factor)));
        if (r != previousReleased || p != previousPressed) {
            candidates.push_back({factor, r, p, pixelBytes(rw, rh, r) + pixelBytes(pw, ph, p)});
            previousReleased = r; previousPressed = p;
        }
        if (factor >= appearance.maximumMagnification || (r == 256 && p == 256)) break;
    }
    constexpr std::size_t budget = 16u * 1024u * 1024u;
    const auto total = [&] { std::size_t bytes = 0; for (const auto& c : candidates) bytes += c.bytes; return bytes; };
    while (total() > budget && candidates.size() > 1) {
        std::vector<Candidate> smaller;
        for (std::size_t i = 1; i < candidates.size(); i += 2) smaller.push_back(candidates[i]);
        if (smaller.empty() || smaller.back().factor != candidates.back().factor) smaller.push_back(candidates.back());
        candidates = std::move(smaller);
    }
    output.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        MagnifiedArrow level; level.factor = candidate.factor;
        level.released = makeReleased(candidate.releasedSize, false);
        if (!level.released) return false;
        level.pressed = makePressed(level.released, candidate.pressedSize);
        if (!level.pressed) return false;
        if (appearance.reverse) std::swap(level.released, level.pressed);
        output.push_back(std::move(level));
    }
    return true;
}

bool BuildArtwork(std::vector<CursorSlot>& slots, const CursorAppearance& requested,
                  std::vector<MagnifiedArrow>& magnifications, std::wstring& error) {
    if (requested.style < 0 || requested.style > 3 ||
        (requested.size != 0 && (requested.size < 24 || requested.size > 256)) ||
        requested.scale < 100 || requested.scale > 600 || requested.opacity < 30 || requested.opacity > 100 ||
        !std::isfinite(requested.maximumMagnification) || requested.maximumMagnification < 1.f ||
        requested.maximumMagnification > 4.f) {
        error = L"Choose a valid cursor style, a cursor scale from 100 to 600 percent and an opacity from 30 to 100 percent.";
        return false;
    }
    const auto arrowSlot = std::find_if(slots.begin(), slots.end(),
        [](const CursorSlot& slot) { return slot.id == kNormalCursor; });
    const bool hasArrow = arrowSlot != slots.end();
    // A scale is measured from the size Windows draws the arrow at, so 100%
    // always means "unchanged", whatever pointer size or DPI is in use.
    CursorAppearance appearance = requested;
    if (!appearance.size && appearance.scale != 100) {
        int base = 32, width = 0, height = 0; DWORD hotX = 0, hotY = 0;
        std::wstring ignored;
        if (hasArrow && CursorDimensions(arrowSlot->original, width, height, hotX, hotY, ignored)) base = std::max(width, height);
        appearance.size = std::clamp(static_cast<int>(std::lround(base * appearance.scale / 100.0)), 24, 256);
    }
    if (appearance.style != 0 && !hasArrow) {
        error = L"The current arrow is animated or unavailable. Select a static Windows arrow before using an arrow preset.";
        return false;
    }
    for (auto& slot : slots) {
        const bool customArrow = slot.id == kNormalCursor && appearance.style != 0;
        if (slot.animated && !customArrow) {
            // Keep the animation: Windows reloads the file at the requested size
            // (its own frames, stretched beyond the largest one). The role is
            // neither inverted nor magnified, and stays untouched at source size.
            slot.released = nullptr; slot.inverted = nullptr; slot.baselineChanged = false; slot.animatedSize = 0;
            slot.animation.clear();
            if (LookActive(appearance) && !slot.sourcePath.empty()) {
                int size = appearance.size, width = 0, height = 0; DWORD hotX = 0, hotY = 0;
                std::wstring ignored;
                if (!size && CursorDimensions(slot.original, width, height, hotX, hotY, ignored)) size = std::max(width, height);
                auto styled = size ? AnimationWithLook(slot.sourcePath, size, appearance) : std::vector<std::uint8_t>{};
                const HCURSOR created = styled.empty() ? nullptr : reinterpret_cast<HCURSOR>(CreateIconFromResourceEx(
                    styled.data(), static_cast<DWORD>(styled.size()), FALSE, 0x00030000, 0, 0, 0));
                if (created) {
                    slot.released = created; slot.animatedSize = size; slot.animation = std::move(styled);
                    slot.baselineChanged = true;
                    continue;
                }
            }
            if (appearance.size && !slot.sourcePath.empty()) {
                const HCURSOR resized = reinterpret_cast<HCURSOR>(LoadImageW(nullptr, slot.sourcePath.c_str(), IMAGE_CURSOR,
                    appearance.size, appearance.size, LR_LOADFROMFILE));
                if (resized) { slot.released = resized; slot.animatedSize = appearance.size; slot.baselineChanged = true; }
            }
            continue;
        }
        StaticArtwork releasedFile, pressedFile;
        bool useReleasedFile = false, usePressedFile = false;
        int nativeSize = 0;
        if (customArrow && appearance.style == 1) {
            const auto path = LocalWindowsCursor(L"aero_arrow.cur");
            if (path.empty()) {
                error = L"Could not locate the Windows cursor assets."; return false;
            }
            if (!releasedFile.Read(path, error)) {
                error = L"The Windows default arrow could not be loaded from this computer. " + error +
                        L" Choose Current Windows to use the installed scheme.";
                return false;
            }
            useReleasedFile = true;
        } else if (customArrow && appearance.style == 3) {
            if (appearance.releasedPath.empty()) {
                error = L"Choose a released .cur file for the custom arrow pair."; return false;
            }
            if (!releasedFile.Read(appearance.releasedPath, error)) return false;
            useReleasedFile = true;
            if (!appearance.pressedPath.empty()) {
                if (!pressedFile.Read(appearance.pressedPath, error)) return false;
                usePressedFile = true;
            }
        } else {
            // Registry access and all optional file reads happen only while
            // configuring. A mismatched/missing file falls back to the saved
            // current artwork rather than replacing it with a different theme.
            if (appearance.size || (slot.id == kNormalCursor && appearance.maximumMagnification > 1.f))
                useReleasedFile = releasedFile.ReadMatching(slot.sourcePath, slot.original);
        }
        if (customArrow && (appearance.style == 1 || appearance.style == 2)) {
            int width = 0, height = 0; DWORD x = 0, y = 0;
            if (!CursorDimensions(slot.original, width, height, x, y, error)) return false;
            nativeSize = std::max(width, height);
        }
        const auto makeReleased = [&](int size, bool originalSize) {
            if (customArrow && appearance.style == 2)
                return LoadClassicArrow(size ? size : nativeSize, originalSize, error);
            if (!customArrow && !size) return CopyAtSize(slot.original, 0, error);
            if (useReleasedFile) return releasedFile.Make(size ? size : nativeSize, error);
            return CopyAtSize(slot.original, size, error);
        };
        const auto makePressed = [&](HCURSOR released, int size) {
            return usePressedFile ? pressedFile.Make(size, error) : CreateInvertedCursor(released, error);
        };
        slot.released = makeReleased(appearance.size, appearance.size == 0);
        if (!slot.released) return false;
        slot.inverted = makePressed(slot.released, appearance.size);
        if (!slot.inverted) return false;
        if (appearance.reverse) std::swap(slot.released, slot.inverted);
        slot.baselineChanged = customArrow || appearance.size != 0 || appearance.reverse || LookActive(appearance);
        const std::size_t firstLevel = magnifications.size();
        if (slot.id == kNormalCursor && !BuildMagnification(magnifications, slot, appearance,
                                                          makeReleased, makePressed, error)) return false;
        // The look is applied last, to the plain released/pressed artwork and
        // every shake level, so inversion never inverts a shadow or a color.
        if (!ApplyLook(slot.released, appearance, error) || !ApplyLook(slot.inverted, appearance, error)) return false;
        for (std::size_t level = firstLevel; level < magnifications.size(); ++level)
            if (!ApplyLook(magnifications[level].released, appearance, error) ||
                !ApplyLook(magnifications[level].pressed, appearance, error)) return false;
    }
    return true;
}

void RefreshSharedHandle(CursorSlot& slot) noexcept {
    const HCURSOR refreshed = LoadCursorW(nullptr, MAKEINTRESOURCEW(slot.id));
    if (refreshed && refreshed != slot.shared) {
        slot.previousShared = slot.shared;
        slot.shared = refreshed;
    }
}

bool ReplaceSlot(CursorSlot& slot, HCURSOR source, DWORD& code) noexcept {
    // Copying an animated cursor keeps only one frame, so an animated role is
    // reloaded from its file for every installation instead.
    HCURSOR copy = nullptr;
    if (slot.animated && slot.animatedSize && source == slot.released) {
        copy = !slot.animation.empty()
            ? reinterpret_cast<HCURSOR>(CreateIconFromResourceEx(const_cast<PBYTE>(slot.animation.data()),
                  static_cast<DWORD>(slot.animation.size()), FALSE, 0x00030000, 0, 0, 0))
            : reinterpret_cast<HCURSOR>(LoadImageW(nullptr, slot.sourcePath.c_str(), IMAGE_CURSOR,
                  slot.animatedSize, slot.animatedSize, LR_LOADFROMFILE));
    } else {
        copy = CopyOwnedCursor(source);
    }
    if (!copy) {
        code = GetLastError();
        return false;
    }
    // SetSystemCursor takes ownership; never hand it either cached snapshot.
    if (!SetSystemCursor(copy, slot.id)) {
        code = GetLastError();
        return false;
    }
    RefreshSharedHandle(slot);
    return true;
}

bool ApplySlot(CursorSlot& slot, HCURSOR source, DWORD& code) noexcept {
    if (slot.applied == source || (source == slot.original && !slot.touched)) return true;
    // Mark before the API call so a partial failure still gets restoration.
    slot.touched = true;
    if (!ReplaceSlot(slot, source, code)) return false;
    slot.applied = source;
    slot.touched = source != slot.original;
    return true;
}

}  // namespace

bool FindCursorCompanion(const std::wstring& selected, std::wstring& companion,
                         bool& selectedIsPressed) {
    companion.clear();
    selectedIsPressed = false;
    const auto separator = selected.find_last_of(L"\\/");
    const std::size_t begin = separator == std::wstring::npos ? 0 : separator + 1;
    if (selected.size() < begin + 5) return false;
    std::wstring name = selected.substr(begin);
    std::transform(name.begin(), name.end(), name.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (name.substr(name.size() - 4) != L".cur") return false;
    const DWORD selectedAttributes = GetFileAttributesW(selected.c_str());
    if (selectedAttributes == INVALID_FILE_ATTRIBUTES || (selectedAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    name.resize(name.size() - 4);
    struct Pair { const wchar_t* released; const wchar_t* pressed; };
    constexpr Pair pairs[] = {{L"normal", L"pressed"}, {L"released", L"pressed"},
        {L"up", L"down"}, {L"white", L"black"}, {L"light", L"dark"}};
    std::wstring found;
    bool pressed = false;
    for (const auto& pair : pairs) for (unsigned side = 0; side < 2; ++side) {
        const std::wstring suffix = side ? pair.pressed : pair.released;
        if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        const auto offset = name.size() - suffix.size();
        if (offset && name[offset - 1] != L'_' && name[offset - 1] != L'-' &&
            name[offset - 1] != L' ' && name[offset - 1] != L'.') continue;
        const auto candidate = selected.substr(0, begin + offset) + (side ? pair.released : pair.pressed) + L".cur";
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!found.empty() && CompareStringOrdinal(found.c_str(), -1, candidate.c_str(), -1, TRUE) != CSTR_EQUAL)
            return false;
        found = candidate;
        pressed = side != 0;
    }
    if (found.empty()) return false;
    companion = std::move(found);
    selectedIsPressed = pressed;
    return true;
}

struct CursorEngine::Impl {
    std::vector<CursorSlot> slots;
    std::vector<MagnifiedArrow> magnifications;
    CursorAppearance appearance;
    std::size_t magnification = 0; // Zero selects each role's normal shape.
    bool initialized = false;
    bool active = false;
    bool pressed = false;

    HCURSOR Desired(const CursorSlot& slot) const noexcept {
        // Press and shake effects never touch an animated role.
        if (slot.animated) return slot.baselineChanged ? slot.released : slot.original;
        if (magnification) {
            const auto& level = magnifications[magnification - 1];
            return pressed ? level.pressed : level.released;
        }
        return pressed ? slot.inverted : slot.baselineChanged ? slot.released : slot.original;
    }

    CursorSlot* FindVisibleCurrent() noexcept {
        CURSORINFO current{};
        current.cbSize = sizeof(current);
        if (!GetCursorInfo(&current) || !(current.flags & CURSOR_SHOWING) ||
            !current.hCursor) return nullptr;
        for (auto& slot : slots) {
            if (current.hCursor == slot.shared ||
                current.hCursor == slot.previousShared) return &slot;
        }
        // Handle replacement can create a new shared handle on some systems.
        // Refresh only on a miss, keeping the usual click path small.
        for (auto& slot : slots) {
            RefreshSharedHandle(slot);
            if (current.hCursor == slot.shared) return &slot;
        }
        return nullptr;
    }
};

CursorEngine::CursorEngine() : impl_(std::make_unique<Impl>()) {}

CursorEngine::~CursorEngine() {
    std::wstring ignored;
    Restore(ignored);
}

bool CursorEngine::Initialize(std::wstring& error) {
    error.clear();
    if (impl_->initialized) return true;
    std::vector<CursorSlot> slots;
    std::vector<MagnifiedArrow> magnifications;
    if (!CaptureOriginals(slots, error)) return false;
    if (!BuildArtwork(slots, impl_->appearance, magnifications, error)) return false;
    impl_->slots = std::move(slots);
    impl_->magnifications = std::move(magnifications);
    impl_->initialized = true;
    impl_->active = false;
    impl_->pressed = false;
    return true;
}

bool CursorEngine::Configure(const CursorAppearance& appearance, std::wstring& error) {
    error.clear();
    std::vector<CursorSlot> prepared;
    std::vector<MagnifiedArrow> magnifications;
    if (!impl_->initialized) {
        // Reload can fail because a previously selected file disappeared.
        // Rebuild the newly requested profile directly, not the stale one.
        if (!CaptureOriginals(prepared, error)) return false;
    } else {
        prepared.reserve(impl_->slots.size());
        for (const auto& saved : impl_->slots) {
            CursorSlot slot;
            slot.id = saved.id; slot.shared = saved.shared; slot.previousShared = saved.previousShared;
            slot.sourcePath = saved.sourcePath; slot.animated = saved.animated;
            // Configuration must never recapture an installed profile as the
            // user original. Clone only our original owned snapshot.
            slot.original = CopyOwnedCursor(saved.original);
            if (!slot.original) {
                error = WinError(L"Could not prepare the original cursor snapshot", GetLastError());
                return false;
            }
            prepared.push_back(std::move(slot));
        }
    }
    if (!BuildArtwork(prepared, appearance, magnifications, error)) return false;
    if (!Restore(error)) return false;
    for (auto& slot : prepared) RefreshSharedHandle(slot);
    impl_->slots = std::move(prepared);
    impl_->magnifications = std::move(magnifications);
    impl_->appearance = appearance;
    impl_->initialized = true;
    return true;
}

bool CursorEngine::SetActive(bool active, std::wstring& error) {
    error.clear();
    if (!active) return Restore(error);
    if (!impl_->initialized && !Initialize(error)) return false;
    if (impl_->active) return true;
    impl_->active = true;
    for (auto& slot : impl_->slots) {
        if (!slot.baselineChanged) continue;
        DWORD code = ERROR_SUCCESS;
        if (!ApplySlot(slot, slot.released, code)) {
            const std::wstring failure = WinError(L"Could not apply the released cursor profile", code);
            std::wstring restoreError;
            Restore(restoreError);
            error = failure;
            if (!restoreError.empty()) error += L"\n" + restoreError;
            return false;
        }
    }
    return true;
}

bool CursorEngine::SetPressed(bool pressed, std::wstring& error) {
    error.clear();
    if (pressed && !SetActive(true, error)) return false;
    impl_->pressed = pressed;
    for (auto& slot : impl_->slots) {
        if (!slot.pressedApplied) continue;
        DWORD code = ERROR_SUCCESS;
        if (!ApplySlot(slot, impl_->active ? impl_->Desired(slot) : slot.original, code)) {
            const std::wstring failure = WinError(L"Could not update the cursor appearance", code);
            std::wstring restoreError;
            Restore(restoreError);
            error = failure;
            if (!restoreError.empty()) error += L"\n" + restoreError;
            return false;
        }
        slot.pressedApplied = impl_->active && (impl_->pressed || impl_->magnification != 0);
    }
    return ApplyCurrent(error);
}

bool CursorEngine::SetMagnification(float factor, std::wstring& error) {
    error.clear();
    if (!std::isfinite(factor)) { error = L"Choose a finite cursor magnification."; return false; }
    if (!impl_->active || !impl_->initialized) return true;
    factor = std::clamp(factor, 1.f, impl_->appearance.maximumMagnification);
    std::size_t selected = 0;
    float distance = factor - 1.f;
    for (std::size_t i = 0; i < impl_->magnifications.size(); ++i) {
        const float candidate = std::abs(impl_->magnifications[i].factor - factor);
        if (candidate <= distance) { distance = candidate; selected = i + 1; }
    }
    if (selected == impl_->magnification) return ApplyCurrent(error);
    impl_->magnification = selected;
    for (auto& slot : impl_->slots) {
        if (!slot.pressedApplied) continue;
        DWORD code = ERROR_SUCCESS;
        if (!ApplySlot(slot, impl_->Desired(slot), code)) {
            const std::wstring failure = WinError(L"Could not resize the locator cursor", code);
            std::wstring restoreError;
            Restore(restoreError);
            error = failure;
            if (!restoreError.empty()) error += L"\n" + restoreError;
            return false;
        }
        slot.pressedApplied = impl_->pressed || impl_->magnification != 0;
    }
    return ApplyCurrent(error);
}

bool CursorEngine::ApplyCurrent(std::wstring& error) {
    error.clear();
    if ((!impl_->pressed && !impl_->magnification) || !impl_->active || !impl_->initialized) return true;
    CursorSlot* slot = impl_->FindVisibleCurrent();
    if (!slot || slot->animated) return true;
    DWORD code = ERROR_SUCCESS;
    // Mark before the replacement so even an unusual partial API failure gets
    // a restoration attempt. Subsequent cursor events are idempotent.
    slot->pressedApplied = true;
    if (!ApplySlot(*slot, impl_->Desired(*slot), code)) {
        const std::wstring failure = WinError(L"Could not apply the cursor effect", code);
        std::wstring restoreError;
        Restore(restoreError);
        error = failure;
        if (!restoreError.empty()) error += L"\n" + restoreError;
        return false;
    }
    return true;
}

bool CursorEngine::Restore(std::wstring& error) noexcept {
    impl_->active = false;
    impl_->pressed = false;
    impl_->magnification = 0;
    DWORD firstFailure = ERROR_SUCCESS;
    try {
        error.clear();
        bool animatedTouched = false;
        for (auto& slot : impl_->slots) {
            if (!slot.touched) continue;
            // An animated role has no faithful snapshot; the scheme reload
            // below brings its animation back exactly as Windows loads it.
            if (slot.animated) { animatedTouched = true; continue; }
            DWORD code = ERROR_SUCCESS;
            if (ReplaceSlot(slot, slot.original, code)) {
                slot.touched = false;
                slot.pressedApplied = false;
                slot.applied = nullptr;
            } else if (!firstFailure) {
                firstFailure = code ? code : ERROR_GEN_FAILURE;
            }
        }
        if (!firstFailure && !animatedTouched) return true;
        const bool restoredScheme = RestoreSystemScheme();
        if (restoredScheme) {
            for (auto& slot : impl_->slots) {
                slot.touched = false;
                slot.pressedApplied = false;
                slot.applied = nullptr;
                RefreshSharedHandle(slot);
            }
            if (!firstFailure) return true;
        } else if (!firstFailure) {
            firstFailure = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
        }
        error = WinError(L"Could not restore a saved cursor", firstFailure);
        error += restoredScheme
            ? L". Your saved Windows cursor scheme was reloaded instead."
            : L". Use Restore Windows cursors or sign out to reset the cursor scheme.";
        return false;
    } catch (...) {
        // Restoration must remain available even during an allocation failure.
        const bool restored = RestoreSystemScheme();
        if (restored) {
            for (auto& slot : impl_->slots) {
                slot.touched = false; slot.pressedApplied = false; slot.applied = nullptr;
            }
        }
        error.clear();
        return false;
    }
}

bool CursorEngine::Reload(std::wstring& error) {
    if (!Restore(error)) return false;
    impl_->slots.clear();
    impl_->magnifications.clear();
    impl_->initialized = false;
    return Initialize(error);
}

bool CursorEngine::IsPressed() const { return impl_->pressed; }

std::size_t CursorEngine::CursorCount() const { return impl_->slots.size(); }

HCURSOR CursorEngine::CopyPreview(bool pressed) const {
    if (impl_->magnification) {
        const auto& level = impl_->magnifications[impl_->magnification - 1];
        return CopyOwnedCursor(pressed ? level.pressed : level.released);
    }
    for (const auto& slot : impl_->slots) {
        if (slot.id == kNormalCursor)
            return CopyOwnedCursor(pressed ? slot.inverted : slot.released);
    }
    return nullptr;
}

bool CursorEngine::RestoreSystemScheme() noexcept {
    return SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0) != FALSE;
}

}  // namespace clickshade
