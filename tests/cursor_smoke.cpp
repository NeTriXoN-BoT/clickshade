// Run explicitly with --run in an interactive Windows desktop, with ClickShade
// closed. This test temporarily changes standard cursor images and restores them.
// Wine can validate pixel conversion and object ownership, but Wine 9 does not
// implement the shared-system cursor mutation that this app relies on.
#include "cursor_engine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>

namespace {
int passed = 0;
int failed = 0;
int skipped = 0;

void Check(bool condition, const char* description) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
    condition ? ++passed : ++failed;
}

void Require(bool condition, const char* description) {
    Check(condition, description);
    if (!condition) throw std::runtime_error(description);
}

void Skip(const char* description) {
    ++skipped;
    std::printf("SKIP %s\n", description);
}

struct OwnedCursor {
    HCURSOR value = nullptr;
    explicit OwnedCursor(HCURSOR cursor) : value(cursor) {}
    ~OwnedCursor() { if (value) DestroyCursor(value); }
    OwnedCursor(const OwnedCursor&) = delete;
    OwnedCursor& operator=(const OwnedCursor&) = delete;
};

struct BitmapSnapshot {
    LONG width = 0;
    LONG height = 0;
    std::vector<std::uint8_t> bgra;
    bool operator==(const BitmapSnapshot& other) const {
        return width == other.width && height == other.height && bgra == other.bgra;
    }
};

BitmapSnapshot ReadBitmap(HBITMAP bitmap) {
    BitmapSnapshot result;
    if (!bitmap) return result;
    BITMAP object{};
    if (!GetObjectW(bitmap, sizeof(object), &object))
        throw std::runtime_error("GetObjectW failed while inspecting a cursor bitmap");
    result.width = object.bmWidth;
    result.height = std::abs(object.bmHeight);
    if (result.width <= 0 || result.height <= 0 ||
        result.width > 4096 || result.height > 8192)
        throw std::runtime_error("Invalid cursor bitmap dimensions");
    result.bgra.resize(static_cast<std::size_t>(result.width) * result.height * 4);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = result.width;
    info.bmiHeader.biHeight = -result.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    if (!dc) throw std::runtime_error("GetDC failed while inspecting a cursor bitmap");
    const int copied = GetDIBits(dc, bitmap, 0, static_cast<UINT>(result.height),
                                result.bgra.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (copied != result.height)
        throw std::runtime_error("GetDIBits failed while inspecting a cursor bitmap");
    return result;
}

struct CursorSnapshot {
    DWORD xHotspot = 0;
    DWORD yHotspot = 0;
    BitmapSnapshot mask;
    BitmapSnapshot color;
    bool operator==(const CursorSnapshot& other) const {
        return xHotspot == other.xHotspot && yHotspot == other.yHotspot &&
               mask == other.mask && color == other.color;
    }
};

CursorSnapshot ReadCursor(HCURSOR cursor) {
    ICONINFO info{};
    if (!cursor || !GetIconInfo(cursor, &info))
        throw std::runtime_error("GetIconInfo failed while inspecting a cursor");
    CursorSnapshot result;
    try {
        result.xHotspot = info.xHotspot;
        result.yHotspot = info.yHotspot;
        result.mask = ReadBitmap(info.hbmMask);
        result.color = ReadBitmap(info.hbmColor);
    } catch (...) {
        if (info.hbmMask) DeleteObject(info.hbmMask);
        if (info.hbmColor) DeleteObject(info.hbmColor);
        throw;
    }
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    return result;
}

class TestWindow {
public:
    TestWindow() {
        previousForeground_ = GetForegroundWindow();
        GetCursorPos(&previousPosition_);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ClickShade.CursorSmoke.Window";
        RegisterClassW(&wc);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName,
            L"ClickShade cursor test", WS_OVERLAPPEDWINDOW,
            40, 40, 300, 180, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window_) throw std::runtime_error("Could not create the test window");
        ShowWindow(window_, SW_SHOWNORMAL);
        SetForegroundWindow(window_);
        POINT position{60, 60};
        ClientToScreen(window_, &position);
        SetCursorPos(position.x, position.y);
        Pump();
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    ~TestWindow() {
        if (window_) DestroyWindow(window_);
        SetCursorPos(previousPosition_.x, previousPosition_.y);
        if (previousForeground_ && IsWindow(previousForeground_))
            SetForegroundWindow(previousForeground_);
    }
    void Pump() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
private:
    HWND window_ = nullptr;
    HWND previousForeground_ = nullptr;
    POINT previousPosition_{};
};

void TestPixelConversion() {
    // Explicit color examples cover white/black, antialiased edges, transparent
    // hidden RGB, colored themes, and invalid input clamping independently of
    // the Win32 cursor object code.
    // Straight alpha, as Windows stores cursor bitmaps: a translucent white
    // edge becomes a translucent black edge, not a darker grey.
    std::array<std::uint8_t, 28> samples{
        255,255,255,255, 0,0,0,255, 255,255,255,128, 0,0,0,64,
        12,34,56,0, 10,20,30,128, 200,1,0,64,
    };
    const std::array<std::uint8_t, 28> expected{
        0,0,0,255, 255,255,255,255, 0,0,0,128, 255,255,255,64,
        12,34,56,0, 245,235,225,128, 55,254,255,64,
    };
    clickshade::InvertCursorBgra(samples.data(), samples.size() / 4);
    Check(samples == expected, "known BGRA colors and antialiased pixels transform correctly");

    std::vector<std::uint8_t> pixels;
    for (unsigned alpha = 0; alpha <= 255; ++alpha) {
        for (unsigned value = 0; value <= 255; value += 3) {
            pixels.insert(pixels.end(), {
                static_cast<std::uint8_t>(value),
                static_cast<std::uint8_t>(255 - value),
                static_cast<std::uint8_t>(alpha / 2),
                static_cast<std::uint8_t>(alpha),
            });
        }
    }
    const auto original = pixels;
    clickshade::InvertCursorBgra(pixels.data(), pixels.size() / 4);
    bool alphaPreserved = true, hiddenKept = true;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        alphaPreserved &= pixels[i + 3] == original[i + 3];
        if (!original[i + 3]) hiddenKept &= std::equal(pixels.begin() + i, pixels.begin() + i + 3, original.begin() + i);
    }
    Check(alphaPreserved, "every tested alpha value remains unchanged");
    Check(hiddenKept, "fully transparent pixels keep their hidden RGB data");
    clickshade::InvertCursorBgra(pixels.data(), pixels.size() / 4);
    Check(pixels == original, "two inversions restore every sample exactly");
}

void TestPreviews(clickshade::CursorEngine& engine) {
    OwnedCursor released(engine.CopyPreview(false));
    OwnedCursor pressed(engine.CopyPreview(true));
    Require(released.value && pressed.value, "both independent preview cursor handles exist");
    const auto a = ReadCursor(released.value);
    const auto b = ReadCursor(pressed.value);
    Check(a.xHotspot == b.xHotspot && a.yHotspot == b.yHotspot,
          "pressed cursor preserves the exact click hotspot");
    Check(a.mask.width == b.mask.width && a.mask.height == b.mask.height &&
          a.color.width == b.color.width && a.color.height == b.color.height,
          "pressed cursor preserves image dimensions");
    Check(!(a == b), "pressed preview differs visibly from released preview");
    if (!a.color.bgra.empty()) {
        Check(a.mask == b.mask, "color cursor retains its original transparency mask");
        bool equalAlpha = a.color.bgra.size() == b.color.bgra.size();
        if (equalAlpha) {
            for (std::size_t i = 3; i < a.color.bgra.size(); i += 4)
                equalAlpha &= a.color.bgra[i] == b.color.bgra[i];
        }
        Check(equalAlpha, "all preview alpha values remain unchanged");
    } else {
        const auto count = a.mask.bgra.size() / 2;
        Check(a.mask.bgra.size() == b.mask.bgra.size() &&
              std::equal(a.mask.bgra.begin(), a.mask.bgra.begin() + count, b.mask.bgra.begin()),
              "monochrome cursor retains its AND transparency plane");
    }
}

CursorSnapshot Preview(clickshade::CursorEngine& engine, bool pressed) {
    OwnedCursor cursor(engine.CopyPreview(pressed));
    if (!cursor.value) throw std::runtime_error("Missing profile preview");
    return ReadCursor(cursor.value);
}

struct CursorFixtures {
    std::wstring prefix, released, pressed, malformed, animated;
    std::vector<std::wstring> additional;
    CursorFixtures() {
        wchar_t directory[MAX_PATH]{}, unique[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"csp", 0, unique))
            throw std::runtime_error("Could not create cursor fixture paths");
        DeleteFileW(unique);
        prefix = unique;
        released = prefix + L"-released.cur";
        pressed = prefix + L"-pressed.cur";
        malformed = prefix + L"-malformed.cur";
        animated = prefix + L"-animated.ani";
        Write(released, Image(24, 80, 180));
        Write(pressed, Image(200, 100, 20));
        Write(malformed, std::vector<std::uint8_t>{'R','I','F','F',0,0,0,0,'A','C','O','N'});
        Write(animated, Image(24, 80, 180));
    }
    ~CursorFixtures() {
        DeleteFileW(released.c_str()); DeleteFileW(pressed.c_str());
        DeleteFileW(malformed.c_str()); DeleteFileW(animated.c_str());
        for (const auto& path : additional) DeleteFileW(path.c_str());
    }
    std::wstring Add(const wchar_t* suffix, const std::vector<std::uint8_t>& bytes) {
        const auto path = prefix + L"-" + suffix;
        Write(path, bytes); additional.push_back(path); return path;
    }
    static void Write(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not create cursor fixture");
        DWORD written = 0;
        const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                        written == bytes.size();
        CloseHandle(file);
        if (!ok) throw std::runtime_error("Could not write cursor fixture");
    }
    static std::vector<std::uint8_t> Image(unsigned blue, unsigned green, unsigned red,
                                          unsigned width = 32, unsigned height = 16,
                                          unsigned hotX = 3, unsigned hotY = 5) {
        // An independently encoded non-square CUR with a noncentral hotspot,
        // opaque center, translucent edge and transparent margin.
        const unsigned stride = ((width + 31) / 32) * 4;
        const unsigned bitmapSize = 40 + width * height * 4 + stride * height;
        std::vector<std::uint8_t> out(22 + bitmapSize, 0);
        const auto u16 = [&](unsigned at, unsigned value) {
            out[at] = static_cast<std::uint8_t>(value); out[at + 1] = static_cast<std::uint8_t>(value >> 8);
        };
        const auto u32 = [&](unsigned at, unsigned value) {
            for (unsigned n = 0; n < 4; ++n) out[at + n] = static_cast<std::uint8_t>(value >> (n * 8));
        };
        u16(2, 2); u16(4, 1); out[6] = static_cast<std::uint8_t>(width); out[7] = static_cast<std::uint8_t>(height);
        u16(10, hotX); u16(12, hotY); u32(14, bitmapSize); u32(18, 22);
        u32(22, 40); u32(26, width); u32(30, height * 2); u16(34, 1); u16(36, 32);
        u32(42, width * height * 4);
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const bool visible = x >= 2 && x < width - 2 && y >= 2 && y < height - 2;
            const unsigned alpha = !visible ? 0 : (x == 2 || y == 2 ? 128 : 255);
            const unsigned offset = 62 + ((height - 1 - y) * width + x) * 4;
            out[offset] = static_cast<std::uint8_t>(alpha ? blue : 0);
            out[offset + 1] = static_cast<std::uint8_t>(alpha ? green : 0);
            out[offset + 2] = static_cast<std::uint8_t>(alpha ? red : 0);
            out[offset + 3] = static_cast<std::uint8_t>(alpha);
            if (!visible) out[62 + width * height * 4 + (height - 1 - y) * stride + x / 8] |=
                static_cast<std::uint8_t>(0x80u >> (x % 8));
        }
        return out;
    }
    static std::vector<std::uint8_t> PngImage() {
        // Independently encoded 64x32 RGBA PNG, translucent upper/left edge,
        // transparent margin, opaque RGB(30,90,220) center; hotspot 6,10.
        constexpr std::uint8_t png[] = {
            137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,64,0,0,0,32,8,6,0,0,0,
            162,157,126,132,0,0,0,77,73,68,65,84,120,218,237,217,33,1,0,32,0,3,193,37,
            33,24,105,8,74,23,144,24,26,236,196,139,217,179,203,152,59,205,5,192,27,171,
            172,47,192,41,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            64,45,128,119,184,177,11,82,253,255,56,32,129,212,20,0,0,0,0,73,69,78,68,174,66,96,130
        };
        std::vector<std::uint8_t> result(22, 0);
        result[2] = 2; result[4] = 1; result[6] = 64; result[7] = 32;
        result[10] = 6; result[12] = 10; result[14] = sizeof(png); result[18] = 22;
        result.insert(result.end(), std::begin(png), std::end(png));
        return result;
    }
    static std::vector<std::uint8_t> MultiImage(const std::vector<std::vector<std::uint8_t>>& images) {
        std::vector<std::uint8_t> result(6 + images.size() * 16, 0);
        result[2] = 2; result[4] = static_cast<std::uint8_t>(images.size());
        for (std::size_t n = 0; n < images.size(); ++n) {
            const auto offset = static_cast<std::uint32_t>(result.size());
            std::copy_n(images[n].data() + 6, 16, result.data() + 6 + n * 16);
            for (unsigned b = 0; b < 4; ++b) result[6 + n * 16 + 12 + b] = static_cast<std::uint8_t>(offset >> (8 * b));
            result.insert(result.end(), images[n].begin() + 22, images[n].end());
        }
        return result;
    }
    static std::vector<std::uint8_t> SnapshotImage(const CursorSnapshot& snapshot) {
        const bool color = !snapshot.color.bgra.empty();
        const unsigned width = snapshot.mask.width;
        const unsigned height = color ? snapshot.color.height : snapshot.mask.height / 2;
        const unsigned stride = ((width + 31) / 32) * 4;
        const unsigned palette = color ? 0 : 8;
        const unsigned imageBytes = color ? width * height * 4 : stride * height;
        const unsigned bitmapSize = 40 + palette + imageBytes + stride * height;
        std::vector<std::uint8_t> out(22 + bitmapSize, 0);
        const auto u16 = [&](unsigned at, unsigned value) {
            out[at] = static_cast<std::uint8_t>(value); out[at + 1] = static_cast<std::uint8_t>(value >> 8);
        };
        const auto u32 = [&](unsigned at, unsigned value) {
            for (unsigned n = 0; n < 4; ++n) out[at + n] = static_cast<std::uint8_t>(value >> (8 * n));
        };
        u16(2, 2); u16(4, 1); out[6] = static_cast<std::uint8_t>(width); out[7] = static_cast<std::uint8_t>(height);
        u16(10, snapshot.xHotspot); u16(12, snapshot.yHotspot); u32(14, bitmapSize); u32(18, 22);
        u32(22, 40); u32(26, width); u32(30, height * 2); u16(34, 1); u16(36, color ? 32 : 1);
        u32(42, imageBytes);
        if (!color) out[66] = out[67] = out[68] = 255;
        const unsigned first = 62 + palette;
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const auto bit = static_cast<std::uint8_t>(0x80u >> (x % 8));
            if (color) {
                std::copy_n(snapshot.color.bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4,
                    4, out.data() + first + ((height - 1 - y) * width + x) * 4);
            } else if (snapshot.mask.bgra[((y + height) * width + x) * 4]) {
                out[first + (height - 1 - y) * stride + x / 8] |= bit;
            }
            if (snapshot.mask.bgra[(y * width + x) * 4])
                out[first + imageBytes + (height - 1 - y) * stride + x / 8] |= bit;
        }
        return out;
    }
    static std::vector<std::uint8_t> LegacyImage(bool color, bool backgroundXor) {
        CursorSnapshot snapshot; snapshot.xHotspot = 1; snapshot.yHotspot = 2;
        snapshot.mask.width = 32; snapshot.mask.height = color ? 32 : 64;
        snapshot.mask.bgra.assign(static_cast<std::size_t>(32) * snapshot.mask.height * 4, 0);
        if (color) {
            snapshot.color.width = snapshot.color.height = 32;
            snapshot.color.bgra.assign(32 * 32 * 4, 0);
        }
        for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x) {
            const bool opaque = y >= 2 && y <= 28 && x >= 2 && x <= 2 + (y - 2) * 3 / 4;
            const bool invert = backgroundXor && x == 27 && y >= 3 && y <= 6;
            const bool fill = opaque && x > 2 && x < 1 + (y - 2) * 3 / 4 && y < 28;
            const auto index = (y * 32 + x) * 4;
            if (!opaque) snapshot.mask.bgra[index] = 255;
            if (color) {
                snapshot.color.bgra[index] = invert ? 40 : fill ? 220 : 0;
                snapshot.color.bgra[index + 1] = invert ? 70 : fill ? 180 : 0;
                snapshot.color.bgra[index + 2] = invert ? 100 : fill ? 80 : 0;
            } else if (fill || invert) snapshot.mask.bgra[index + 32 * 32 * 4] = 255;
        }
        return SnapshotImage(snapshot);
    }
};

std::array<std::uint8_t, 4> CenterPixel(const CursorSnapshot& snapshot) {
    if (snapshot.color.bgra.empty()) throw std::runtime_error("Expected a color cursor fixture");
    const auto index = (static_cast<std::size_t>(snapshot.color.height / 2) * snapshot.color.width + snapshot.color.width / 2) * 4;
    return {snapshot.color.bgra[index], snapshot.color.bgra[index + 1],
            snapshot.color.bgra[index + 2], snapshot.color.bgra[index + 3]};
}

bool CleanAlpha(const CursorSnapshot& image, bool& antialias) {
    bool visible = false, transparent = false, valid = !image.color.bgra.empty();
    antialias = false;
    for (std::size_t i = 0; i < image.color.bgra.size(); i += 4) {
        const auto* pixel = image.color.bgra.data() + i;
        visible |= pixel[3] == 255; transparent |= pixel[3] == 0;
        antialias |= pixel[3] > 0 && pixel[3] < 255;
        valid &= pixel[3] || (!pixel[0] && !pixel[1] && !pixel[2]);
    }
    return visible && transparent && valid;
}

void TestLegacyAndClassicScaling(clickshade::CursorEngine& engine) {
    CursorFixtures files;
    std::wstring error;
    clickshade::CursorAppearance appearance; appearance.style = 3;
    for (bool color : {false, true}) {
        appearance.releasedPath = files.Add(color ? L"legacy-color.cur" : L"legacy-mono.cur",
            CursorFixtures::LegacyImage(color, false));
        appearance.size = 0;
        Require(engine.Configure(appearance, error), "ordinary legacy cursor decodes at its native size");
        OwnedCursor native(reinterpret_cast<HCURSOR>(LoadImageW(nullptr, appearance.releasedPath.c_str(), IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE)));
        Check(native.value && Preview(engine, false) == ReadCursor(native.value),
              "source-size legacy artwork retains original pixels, mask and hotspot exactly");
        bool allSizes = true, allHotspots = true, allAlpha = true, allPairs = true;
        for (int size : {24, 25, 31, 33, 40, 63, 64, 96, 127, 128}) {
            appearance.size = size;
            if (!engine.Configure(appearance, error)) { allSizes = false; break; }
            const auto light = Preview(engine, false), dark = Preview(engine, true);
            allSizes &= light.color.width == size && light.color.height == size;
            allHotspots &= light.xHotspot == static_cast<DWORD>(MulDiv(1, size, 32)) &&
                           light.yHotspot == static_cast<DWORD>(MulDiv(2, size, 32));
            bool antialias = false;
            allAlpha &= CleanAlpha(light, antialias) && antialias;
            allPairs &= light.mask == dark.mask && light.color.bgra.size() == dark.color.bgra.size();
            for (std::size_t i = 0; i + 3 < light.color.bgra.size(); i += 4) {
                allPairs &= light.color.bgra[i + 3] == dark.color.bgra[i + 3];
                for (unsigned c = 0; c < 3; ++c)
                    allPairs &= light.color.bgra[i + 3]
                        ? std::abs(light.color.bgra[i + c] + dark.color.bgra[i + c] - 255) <= 2
                        : light.color.bgra[i + c] == 0 && dark.color.bgra[i + c] == 0;
            }
        }
        Check(allSizes && allHotspots, "legacy cursor resizing handles odd/even sizes and proportionate hotspots");
        Check(allAlpha, "ordinary legacy masks become clean straight-alpha edges at small and large sizes");
        Check(allPairs, "resized legacy light/dark pair retains identical shape, alpha and click position");

        appearance.releasedPath = files.Add(color ? L"xor-color.cur" : L"xor-mono.cur",
            CursorFixtures::LegacyImage(color, true));
        appearance.size = 0;
        Require(engine.Configure(appearance, error), "background-XOR cursor fixture decodes");
        const auto source = Preview(engine, false);
        appearance.size = 63;
        Require(engine.Configure(appearance, error), "background-XOR cursor resizes without alpha conversion");
        const auto scaled = Preview(engine, false);
        bool planes = scaled.color.bgra.empty() == source.color.bgra.empty();
        bool nonAlpha = true;
        for (int y = 0; y < 63; ++y) for (int x = 0; x < 63; ++x) {
            const int sx = ((x * 2 + 1) * 32) / 126, sy = ((y * 2 + 1) * 32) / 126;
            planes &= scaled.mask.bgra[(y * 63 + x) * 4] == source.mask.bgra[(sy * 32 + sx) * 4];
            if (source.color.bgra.empty()) {
                planes &= scaled.mask.bgra[((y + 63) * 63 + x) * 4] == source.mask.bgra[((sy + 32) * 32 + sx) * 4];
            } else {
                for (unsigned c = 0; c < 3; ++c)
                    planes &= scaled.color.bgra[(y * 63 + x) * 4 + c] == source.color.bgra[(sy * 32 + sx) * 4 + c];
                nonAlpha &= scaled.color.bgra[(y * 63 + x) * 4 + 3] == 0;
            }
        }
        Check(planes && nonAlpha, "true background inversion retains paired AND/XOR states at noninteger scale");
    }

    appearance = {}; appearance.style = 2;
    bool classicSizes = true, classicClean = true, diagonal = true;
    for (int size : {24, 25, 31, 32, 33, 40, 48, 64, 96, 127, 128}) {
        appearance.size = size;
        if (!engine.Configure(appearance, error)) { classicSizes = false; break; }
        const auto cursor = Preview(engine, false);
        classicSizes &= cursor.color.width == size && cursor.color.height == size &&
            cursor.xHotspot == 0 && cursor.yHotspot == 0;
        bool antialias = false;
        classicClean &= CleanAlpha(cursor, antialias) && antialias;
        // The canonical outer edge runs from (0.5,0.5) to (13.4,12.1) on the
        // 32px reference canvas, the classic Windows arrow's proportions. At
        // 128 it must remain a straight edge, rather than four-pixel steps
        // copied from a 32px mask.
        if (size == 128) for (int x = 8; x < 50; ++x) {
            int top = 0;
            while (top < size && cursor.color.bgra[(top * size + x) * 4 + 3] == 0) ++top;
            diagonal &= std::abs(top - (2 + (11.6 / 12.9) * (x - 2))) <= 1.1;
        }
        // The glyph keeps the classic proportion: roughly 13 of 32 units wide.
        if (size == 128) {
            int rightmost = 0;
            for (int y = 0; y < size; ++y) for (int x = size - 1; x > rightmost; --x)
                if (cursor.color.bgra[(y * size + x) * 4 + 3]) { rightmost = x; break; }
            classicSizes &= rightmost >= 48 && rightmost <= 60;
        }
    }
    Check(classicSizes, "Classic uses exact final dimensions, the tip hotspot and classic proportions across the full size range");
    Check(classicClean && diagonal, "large Classic arrow is rasterized with a straight clean diagonal instead of magnified pixel steps");
    Require(engine.Configure({}, error), "legacy and Classic scaling tests return to the original profile");
}

class RegistryCursorSource {
public:
    explicit RegistryCursorSource(const std::wstring& path) {
        DWORD disposition = 0;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key_, &disposition) != ERROR_SUCCESS)
            throw std::runtime_error("Could not open the isolated test cursor registry key");
        createdKey_ = disposition == REG_CREATED_NEW_KEY;
        DWORD bytes = 0;
        const auto found = RegQueryValueExW(key_, L"Arrow", nullptr, &type_, nullptr, &bytes);
        existed_ = found == ERROR_SUCCESS;
        if ((found != ERROR_SUCCESS && found != ERROR_FILE_NOT_FOUND) || bytes > 65536) {
            RegCloseKey(key_); key_ = nullptr; throw std::runtime_error("Could not save cursor registry fixture");
        }
        saved_.resize(bytes);
        if (existed_ && RegQueryValueExW(key_, L"Arrow", nullptr, &type_, saved_.data(), &bytes) != ERROR_SUCCESS) {
            RegCloseKey(key_); key_ = nullptr; throw std::runtime_error("Could not read saved cursor registry fixture");
        }
        if (RegSetValueExW(key_, L"Arrow", 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()),
            static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            RegCloseKey(key_); key_ = nullptr; throw std::runtime_error("Could not set isolated cursor source fixture");
        }
    }
    ~RegistryCursorSource() {
        if (!key_) return;
        if (existed_) RegSetValueExW(key_, L"Arrow", 0, type_, saved_.data(), static_cast<DWORD>(saved_.size()));
        else RegDeleteValueW(key_, L"Arrow");
        RegCloseKey(key_);
        if (createdKey_) RegDeleteKeyW(HKEY_CURRENT_USER, L"Control Panel\\Cursors");
    }
    RegistryCursorSource(const RegistryCursorSource&) = delete;
    RegistryCursorSource& operator=(const RegistryCursorSource&) = delete;
private:
    HKEY key_ = nullptr; DWORD type_ = 0; bool existed_ = false, createdKey_ = false;
    std::vector<BYTE> saved_;
};

void TestRegisteredResolutionSource(clickshade::CursorEngine& engine) {
    std::wstring error;
    Require(engine.Restore(error) && engine.Configure({}, error), "registered-source test begins with original cursor snapshots");
    CursorFixtures files;
    const auto original = Preview(engine, false);
    clickshade::CursorAppearance sized; sized.size = 96;
    CursorSnapshot fallback;
    {
        RegistryCursorSource source(files.prefix + L"-absent.cur");
        clickshade::CursorEngine candidate;
        Require(candidate.Configure(sized, error), "captured-only reference resize prepares when the optional source is absent");
        fallback = Preview(candidate, false);
    }
    const auto larger = CursorFixtures::Image(211, 57, 13, 128, 128, 4, 8);
    const auto matching = files.Add(L"registered-matching.cur", CursorFixtures::MultiImage({CursorFixtures::SnapshotImage(original), larger}));
    {
        RegistryCursorSource source(matching);
        clickshade::CursorEngine candidate;
        Require(candidate.Configure(sized, error), "matching registered multi-resolution source prepares");
        const auto resized = Preview(candidate, false);
        Check(CenterPixel(resized) == std::array<std::uint8_t,4>{211,57,13,255} && resized.color.width == 96,
              "installed theme uses the larger source frame after verifying its native artwork matches the snapshot");
    }
    const auto mismatched = files.Add(L"registered-other.cur", CursorFixtures::MultiImage({CursorFixtures::Image(3,9,17,32,32), larger}));
    {
        RegistryCursorSource source(mismatched);
        clickshade::CursorEngine candidate;
        Require(candidate.Configure(sized, error), "mismatched optional registered source remains recoverable");
        Check(Preview(candidate, false) == fallback,
              "different registered artwork cannot override an accessibility recoloring or current snapshot");
    }
    Require(engine.Configure({}, error), "registered resolution test restores normal source selection");
}

void TestMagnification(clickshade::CursorEngine& engine, bool wine) {
    CursorFixtures files;
    const auto path = files.Add(L"shake.cur", CursorFixtures::MultiImage({
        CursorFixtures::Image(20,40,200), CursorFixtures::Image(30,180,50,64,32,6,10),
        CursorFixtures::Image(210,60,10,128,64,12,20)}));
    clickshade::CursorAppearance appearance; appearance.style = 3;
    appearance.releasedPath = path; appearance.maximumMagnification = 4.f;
    std::wstring error;
    Require(engine.Configure(appearance, error), "shake Arrow levels are prepared with the custom source frames");
    const auto normal = Preview(engine, false), normalPressed = Preview(engine, true);
    Require(engine.SetActive(true, error) && engine.SetMagnification(3.f, error), "prepared3x locator activates without a held button");
    const auto large = Preview(engine, false), largePressed = Preview(engine, true);
    Check(large.color.width == 96 && large.color.height == 48 && large.xHotspot == 9 && large.yHotspot == 15 &&
          CenterPixel(large) == std::array<std::uint8_t,4>{210,60,10,255},
          "magnification uses the original high-resolution source frame and proportional hotspot");
    Require(DeleteFileW(path.c_str()) != FALSE, "shake source can disappear after all levels have been cached");
    bool cycles = true;
    for (int n = 0; n < 32; ++n) {
        cycles &= engine.SetMagnification(2.f, error) && engine.SetPressed(true, error) && engine.ApplyCurrent(error) &&
                  engine.SetMagnification(4.f, error) && engine.SetPressed(false, error) && engine.SetMagnification(3.f, error);
    }
    Check(cycles && Preview(engine, false) == large && Preview(engine, true) == largePressed,
          "cached shake/press cycles require no file access and never progressively rescale or consume artwork");
    if (!wine) {
        const auto beamBefore = ReadCursor(LoadCursorW(nullptr, IDC_IBEAM));
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        Require(engine.ApplyCurrent(error), "native I-beam transition accepts the locator Arrow");
        Check(ReadCursor(LoadCursorW(nullptr, IDC_IBEAM)) == large,
              "native shape transition during unpressed magnification uses the cached Arrow");
        Require(engine.SetMagnification(1.f, error), "native magnification finishes");
        Check(ReadCursor(LoadCursorW(nullptr, IDC_IBEAM)) == beamBefore,
              "ending magnification restores the configured normal role shape");
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    } else Skip("native magnified role transition and restoration: Wine does not mutate shared system cursors");
    Require(engine.SetMagnification(1.f, error), "one-times scale restores normal configured artwork");
    Check(Preview(engine, false) == normal && Preview(engine, true) == normalPressed && !engine.IsPressed(),
          "ending a locator preserves both normal profile states and released button state");
    Require(engine.SetMagnification(4.f, error) && engine.Restore(error), "pause restores originals while a locator is active");
    Check(Preview(engine, false) == normal && engine.SetMagnification(4.f, error) && Preview(engine, false) == normal,
          "inactive engine cannot leave a magnified pointer or reactivate itself");
    Check(!engine.SetMagnification(std::numeric_limits<float>::quiet_NaN(), error) && !error.empty(),
          "invalid non-finite locator scale is rejected safely");
    appearance = {}; appearance.style = 2; appearance.size = 128; appearance.reverse = true;
    appearance.maximumMagnification = 4.f;
    Require(engine.Configure(appearance, error) && engine.SetActive(true, error) && engine.SetMagnification(4.f, error),
            "large reversed Classic profile prepares a bounded locator cache");
    const auto capped = Preview(engine, false), cappedDown = Preview(engine, true);
    bool antialias = false;
    Check(capped.color.width == 256 && capped.color.height == 256 && CleanAlpha(capped, antialias) && antialias,
          "locator caps image dimensions at256 and retains final-resolution clean Classic artwork");
    bool inverse = capped.color.bgra.size() == cappedDown.color.bgra.size();
    for (std::size_t i = 0; i + 3 < capped.color.bgra.size(); i += 4) {
        inverse &= capped.color.bgra[i + 3] == cappedDown.color.bgra[i + 3];
        for (unsigned c = 0; c < 3; ++c)
            inverse &= capped.color.bgra[i + 3]
                ? std::abs(capped.color.bgra[i + c] + cappedDown.color.bgra[i + c] - 255) <= 2
                : capped.color.bgra[i + c] == 0 && cappedDown.color.bgra[i + c] == 0;
    }
    Check(inverse, "reversed locator states keep identical alpha and complementary colors");
    Require(engine.Restore(error) && engine.Configure({}, error), "all locator caches release and the default profile remains selectable");
}

void TestHeldRoleTransitions(clickshade::CursorEngine& engine, bool wine) {
    std::wstring error;
    Require(engine.Configure({}, error) && engine.SetPressed(true, error), "held-role test begins with a real pressed state");
    bool held = true;
    for (const auto id : {IDC_ARROW, IDC_SIZEALL, IDC_SIZENWSE, IDC_SIZEWE, IDC_IBEAM, IDC_ARROW}) {
        SetCursor(LoadCursorW(nullptr, id));
        held &= engine.ApplyCurrent(error) && engine.ApplyCurrent(error) && engine.IsPressed();
    }
    Check(held, "move/resize/shared-role transitions cannot clear a held button or consume a cached cursor");
    CursorFixtures files;
    OwnedCursor privateCursor(reinterpret_cast<HCURSOR>(LoadImageW(nullptr, files.released.c_str(), IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE)));
    const auto privateBefore = ReadCursor(privateCursor.value);
    SetCursor(privateCursor.value);
    Check(engine.ApplyCurrent(error) && engine.IsPressed() && ReadCursor(privateCursor.value) == privateBefore,
          "an application-owned cursor does not reset hold state or get forcibly replaced");
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    Check(engine.ApplyCurrent(error) && engine.IsPressed(), "pressed appearance resumes when an application returns to a supported system role");
    if (!wine) {
        OwnedCursor expected(engine.CopyPreview(true));
        Check(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == ReadCursor(expected.value),
              "native arrow remains pressed after drag-style role transitions");
    }
    Require(engine.SetPressed(false, error) && engine.Restore(error), "held-role transitions restore on the actual release");
}

void TestMultiImageAndCompanions(clickshade::CursorEngine& engine) {
    CursorFixtures files;
    using clickshade::CursorAppearance;
    const auto multi = files.Add(L"multi.cur", CursorFixtures::MultiImage({
        CursorFixtures::Image(20, 40, 200),
        CursorFixtures::Image(30, 180, 50, 64, 32, 6, 10),
        CursorFixtures::Image(210, 60, 10, 128, 64, 12, 20)}));
    CursorAppearance appearance; appearance.style = 3; appearance.releasedPath = multi;
    std::wstring error;
    Require(engine.Configure(appearance, error), "multi-image cursor loads with source-size preservation");
    auto preview = Preview(engine, false);
    Check(preview.color.width == 32 && preview.color.height == 16 &&
          CenterPixel(preview) == std::array<std::uint8_t, 4>{20,40,200,255},
          "source-size mode retains the file's native first-frame size without enlargement");
    appearance.size = 64;
    Require(engine.Configure(appearance, error), "multi-image cursor selects a requested 64-pixel frame");
    preview = Preview(engine, false);
    Check(preview.color.width == 64 && preview.color.height == 32 &&
          CenterPixel(preview) == std::array<std::uint8_t, 4>{30,180,50,255},
          "exact high-resolution frame is used rather than scaling the first low-resolution image");
    Check(preview.xHotspot == 6 && preview.yHotspot == 10,
          "selected high-resolution frame retains its own hotspot");
    appearance.size = 96;
    Require(engine.Configure(appearance, error), "intermediate cursor size loads from a larger frame");
    preview = Preview(engine, false);
    Check(preview.color.width == 96 && preview.color.height == 48 &&
          CenterPixel(preview) == std::array<std::uint8_t, 4>{210,60,10,255},
          "intermediate size downsamples the next larger frame and preserves its non-square shape");
    Check(preview.xHotspot == 9 && preview.yHotspot == 15,
          "downsampling scales the selected frame hotspot proportionally");
    // Every visible pixel of this frame is RGB(10,60,210); filtering in
    // premultiplied space keeps that straight color on translucent edges.
    bool visible = false, transparent = false, translucent = false, clean = true;
    for (std::size_t i = 0; i < preview.color.bgra.size(); i += 4) {
        const auto* p = preview.color.bgra.data() + i;
        visible |= p[3] == 255; transparent |= p[3] == 0; translucent |= p[3] > 0 && p[3] < 255;
        if (!p[3]) clean &= !p[0] && !p[1] && !p[2];
        else if (p[3] >= 64) clean &= std::abs(p[0] - 210) <= 4 && std::abs(p[1] - 60) <= 4 && std::abs(p[2] - 10) <= 4;
    }
    Check(visible && transparent && translucent && clean,
          "alpha-aware resizing preserves visible body, transparent margins and clean translucent edges");
    TestPreviews(engine);
    const auto released = Preview(engine, false), pressed = Preview(engine, true);
    Require(engine.SetActive(true, error) && engine.SetPressed(true, error) && engine.SetPressed(true, error) &&
            engine.ApplyCurrent(error) && engine.SetPressed(false, error) && engine.SetPressed(false, error),
            "repeated pressed/released requests remain idempotent with selected and resized artwork");
    Check(Preview(engine, false) == released && Preview(engine, true) == pressed,
          "cursor installation never consumes or changes the cached images");
    const auto png = files.Add(L"png.cur", CursorFixtures::MultiImage({CursorFixtures::Image(20,40,200), CursorFixtures::PngImage()}));
    appearance.releasedPath = png; appearance.size = 64;
    if (!engine.Configure(appearance, error)) {
        std::fwprintf(stderr, L"PNG fixture error: %ls\n", error.c_str());
        Require(false, "PNG cursor frame decodes through the native resource API");
    } else Check(true, "PNG cursor frame decodes through the native resource API");
    preview = Preview(engine, false);
    Check(preview.color.width == 64 && preview.color.height == 32 && preview.xHotspot == 6 && preview.yHotspot == 10 &&
          CenterPixel(preview) == std::array<std::uint8_t, 4>{220,90,30,255},
          "PNG high-resolution frame retains its true pixels, dimensions and hotspot");
    TestPreviews(engine);
    auto mismatch = CursorFixtures::Image(20,40,200); mismatch[26] = 64;
    appearance.releasedPath = files.Add(L"mismatched.cur", mismatch);
    Check(!engine.Configure(appearance, error) && !error.empty() && Preview(engine, false) == preview,
          "mismatched directory/image dimensions are rejected without replacing the prepared cursor");

    std::wstring companion; bool selectedPressed = false;
    Check(clickshade::FindCursorCompanion(files.released, companion, selectedPressed) &&
          companion == files.pressed && !selectedPressed,
          "released filename discovers one pressed partner in the same directory");
    Check(clickshade::FindCursorCompanion(files.pressed, companion, selectedPressed) &&
          companion == files.released && selectedPressed,
          "selecting a pressed filename identifies its released partner and direction");
    files.Add(L"normal.cur", CursorFixtures::Image(30,40,50));
    Check(!clickshade::FindCursorCompanion(files.pressed, companion, selectedPressed) && companion.empty() && !selectedPressed,
          "ambiguous normal/released partners are never silently selected");
    const auto light = files.Add(L"color-LIGHT.CUR", CursorFixtures::Image(200,210,220));
    const auto dark = files.Add(L"color-dark.cur", CursorFixtures::Image(20,30,40));
    Check(clickshade::FindCursorCompanion(light, companion, selectedPressed) && companion == dark && !selectedPressed,
          "light/dark companion matching ignores case and honors token boundaries");
    const auto unrelated = files.Add(L"abnormal.cur", CursorFixtures::Image(20,30,40));
    Check(!clickshade::FindCursorCompanion(unrelated, companion, selectedPressed) && companion.empty(),
          "ordinary filename substrings and files without a partner are not guessed");
    Require(engine.Restore(error), "resolution and discovery tests restore user cursor snapshots");
    Require(engine.Configure({}, error), "original Windows profile remains selectable after all file tests");
}

void TestProfiles(clickshade::CursorEngine& engine, const CursorSnapshot& original, bool wine) {
    using clickshade::CursorAppearance;
    std::wstring error;
    const CursorAppearance current{};
    Require(engine.Configure(current, error), "current Windows profile prepares without changing its baseline");
    const auto currentReleased = Preview(engine, false), currentPressed = Preview(engine, true);
    Require(engine.SetActive(true, error), "current Windows released profile activates");
    if (!wine) Check(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == original,
                     "unchanged current-Windows release leaves the original system cursor intact");

    CursorAppearance reverse; reverse.reverse = true;
    Require(engine.Configure(reverse, error), "reverse mode configures from saved originals");
    Check(Preview(engine, false) == currentPressed && Preview(engine, true) == currentReleased,
          "reverse mode swaps cached released and pressed artwork exactly");
    Require(engine.SetActive(true, error), "reversed released profile activates");
    if (!wine) Check(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == currentPressed,
                     "native reverse mode is inverted while released");
    Require(engine.SetPressed(true, error), "reversed profile accepts press");
    if (!wine) Check(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == currentReleased,
                     "native reverse mode returns to original artwork while pressed");
    Require(engine.SetPressed(false, error), "reversed profile returns to configured release");
    if (!wine) Check(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == currentPressed,
                     "native reverse release retains its configured baseline");
    Require(engine.SetActive(false, error), "pausing reverse mode restores user originals");
    if (!wine) Check(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == original,
                     "native pause restores original user artwork rather than the profile release");

    CursorAppearance sized; sized.size = 64;
    Require(engine.Configure(sized, error), "installed scheme supports explicit cursor sizing");
    auto sizedPreview = Preview(engine, false);
    const LONG originalWidth = currentReleased.color.width ? currentReleased.color.width : currentReleased.mask.width;
    const LONG originalHeight = currentReleased.color.height ? currentReleased.color.height : currentReleased.mask.height / 2;
    const int longest = std::max(originalWidth, originalHeight);
    const LONG newWidth = std::max(1, MulDiv(originalWidth, 64, longest));
    const LONG newHeight = std::max(1, MulDiv(originalHeight, 64, longest));
    Check(sizedPreview.mask.width == newWidth &&
          (sizedPreview.color.height ? sizedPreview.color.height : sizedPreview.mask.height / 2) == newHeight,
          "resizing keeps the source aspect ratio with a 64-pixel longest dimension");
    Check(sizedPreview.xHotspot == static_cast<DWORD>(MulDiv(currentReleased.xHotspot, newWidth, originalWidth)) &&
          sizedPreview.yHotspot == static_cast<DWORD>(MulDiv(currentReleased.yHotspot, newHeight, originalHeight)),
          "resizing scales the original hotspot proportionally");
    TestPreviews(engine);

    // Scale is measured from the source arrow, so 200 percent doubles it.
    CursorAppearance scaled; scaled.scale = 200;
    Require(engine.Configure(scaled, error), "installed scheme supports a cursor scale");
    const auto scaledPreview = Preview(engine, false);
    const int doubled = std::min(256, longest * 2);
    const LONG scaledWidth = std::max(1, MulDiv(originalWidth, doubled, longest));
    Check(scaledPreview.mask.width == scaledWidth, "a 200 percent scale doubles the arrow's longest dimension");
    scaled.scale = 600;
    Require(engine.Configure(scaled, error), "the largest cursor scale configures");
    const auto largest = Preview(engine, false);
    Check(std::max<LONG>(largest.mask.width, largest.color.height ? largest.color.height : largest.mask.height / 2) <= 256,
          "large scales stay within Windows' 256-pixel cursor limit");
    CursorAppearance outOfRange; outOfRange.scale = 700;
    Check(!engine.Configure(outOfRange, error) && !error.empty(), "a cursor scale above 600 percent is rejected");

    CursorAppearance classic; classic.style = 2; classic.size = 64;
    Require(engine.Configure(classic, error), "local classic arrow or independent fallback configures at 64 pixels");
    const auto classicReleased = Preview(engine, false), classicPressed = Preview(engine, true);
    Check(classicReleased.mask.width == 64 &&
          (classicReleased.color.height ? classicReleased.color.height : classicReleased.mask.height / 2) == 64,
          "classic arrow uses the requested dimensions");
    bool white = false, black = false, transparent = false;
    for (std::size_t n = 0; n + 3 < classicReleased.color.bgra.size(); n += 4) {
        const auto* p = classicReleased.color.bgra.data() + n;
        white |= p[3] == 255 && p[0] == 255 && p[1] == 255 && p[2] == 255;
        black |= p[3] == 255 && p[0] == 0 && p[1] == 0 && p[2] == 0;
        transparent |= p[3] == 0;
    }
    if (classicReleased.color.bgra.empty()) {
        const auto half = classicReleased.mask.bgra.size() / 2;
        for (std::size_t i = 0; i < half; i += 4) {
            const bool andBit = classicReleased.mask.bgra[i] != 0;
            const bool xorBit = classicReleased.mask.bgra[i + half] != 0;
            white |= !andBit && xorBit; black |= !andBit && !xorBit;
            transparent |= andBit && !xorBit;
        }
    }
    Check(white && black && transparent, "classic released arrow has white fill, black outline and transparency");
    classic.reverse = true;
    Require(engine.Configure(classic, error), "classic reverse profile configures");
    Check(Preview(engine, false) == classicPressed && Preview(engine, true) == classicReleased,
          "classic reverse mode swaps white and black artwork");
    Require(engine.SetActive(true, error) && engine.SetPressed(true, error), "classic profile activates and presses");
    Require(engine.Configure(current, error), "reconfiguration while pressed succeeds");
    Check(!engine.IsPressed() && Preview(engine, false) == currentReleased,
          "configuration never recaptures an active profile as the user original");

    CursorFixtures files;
    CursorAppearance custom; custom.style = 3; custom.releasedPath = files.released;
    custom.pressedPath = files.pressed;
    Require(engine.Configure(custom, error), "custom static released/pressed pair loads");
    OwnedCursor fileReleased(reinterpret_cast<HCURSOR>(LoadImageW(nullptr, files.released.c_str(), IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE)));
    OwnedCursor filePressed(reinterpret_cast<HCURSOR>(LoadImageW(nullptr, files.pressed.c_str(), IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE)));
    Check(Preview(engine, false) == ReadCursor(fileReleased.value) && Preview(engine, true) == ReadCursor(filePressed.value),
          "custom pair preserves each selected file's pixels, dimensions and hotspot");
    auto customOriginal = Preview(engine, false);
    Check(customOriginal.color.width == 32 && customOriginal.color.height == 16 &&
          customOriginal.xHotspot == 3 && customOriginal.yHotspot == 5,
          "custom non-square cursor retains native dimensions and click tip");
    custom.pressedPath.clear();
    Require(engine.Configure(custom, error), "custom arrow can generate pressed artwork automatically");
    TestPreviews(engine);
    custom.size = 64;
    Require(engine.Configure(custom, error), "custom arrow pair supports explicit size");
    const auto customScaledRelease = Preview(engine, false), customScaledPress = Preview(engine, true);
    Check(customScaledRelease.color.width == 64 && customScaledRelease.color.height == 32 &&
          customScaledRelease.xHotspot == 6 && customScaledRelease.yHotspot == 10,
          "custom size changes dimensions and hotspot without stretching its aspect ratio");
    custom.reverse = true;
    Require(engine.Configure(custom, error), "custom reverse mode configures");
    Check(Preview(engine, false) == customScaledPress && Preview(engine, true) == customScaledRelease,
          "custom reverse mode swaps the scaled artwork exactly");
    Require(engine.SetActive(true, error) && engine.SetPressed(true, error), "custom profile activates and presses");
    CursorAppearance missing = custom; missing.releasedPath += L"-missing.cur";
    Check(!engine.Configure(missing, error) && !error.empty() && engine.IsPressed() &&
          Preview(engine, false) == customScaledPress,
          "missing-file configuration leaves the previous prepared profile intact");
    CursorAppearance invalid = custom; invalid.releasedPath = files.malformed;
    Check(!engine.Configure(invalid, error) && !error.empty(), "malformed cursor files are rejected before applying artwork");
    invalid.releasedPath = files.animated;
    Check(!engine.Configure(invalid, error) && error.find(L".ani") != std::wstring::npos,
          "animated custom files are explicitly rejected instead of silently flattened");
    invalid = current; invalid.size = 500;
    Check(!engine.Configure(invalid, error) && !error.empty(), "out-of-range cursor size is rejected");
    Require(engine.Restore(error), "custom and resized profiles restore the original user scheme");
    if (!wine) Check(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == original,
                     "native custom-profile exit restores the exact original cursor");

    Require(DeleteFileW(files.released.c_str()) != FALSE, "loaded custom source can be removed after caching");
    Check(!engine.Reload(error) && !error.empty(), "reload reports a custom cursor file removed after configuration");
    Require(engine.Configure(current, error), "selecting Current Windows recovers from a failed custom reload without restarting");
    Check(Preview(engine, false) == currentReleased, "failed reload recovery retains the actual original user cursor");

    CursorAppearance windowsDefault; windowsDefault.style = 1;
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
    const auto asset = std::wstring(windows) + L"\\Cursors\\aero_arrow.cur";
    const bool assetExists = GetFileAttributesW(asset.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool loadedDefault = engine.Configure(windowsDefault, error);
    if (assetExists) {
        Require(loadedDefault, "Windows default arrow loads from the actual local OS asset");
        TestPreviews(engine);
    } else {
        Check(!loadedDefault && !error.empty(), "missing Windows default assets report a recoverable error");
        Skip("Windows default artwork loading: this test system has no aero_arrow.cur asset");
    }
    Require(engine.Configure(current, error), "current Windows scheme can be selected again after every profile");
    Check(Preview(engine, false) == currentReleased, "profile switching retains the original user baseline exactly");
    if (wine) Skip("native profile baseline/press/pause mutation: Wine 9 SetSystemCursor does not mutate shared images");
}

// Draws a cursor over a uniform background the way Windows composites it.
std::vector<std::uint8_t> Rendered(HCURSOR cursor, LONG width, LONG height, std::uint8_t background) {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP target = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!target || !memory || !bits) {
        if (target) DeleteObject(target);
        if (memory) DeleteDC(memory);
        throw std::runtime_error("Could not allocate a cursor rendering surface");
    }
    const HGDIOBJ previous = SelectObject(memory, target);
    std::fill_n(static_cast<std::uint8_t*>(bits), static_cast<std::size_t>(width) * height * 4, background);
    DrawIconEx(memory, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
    GdiFlush();
    std::vector<std::uint8_t> result(static_cast<std::uint8_t*>(bits),
                                     static_cast<std::uint8_t*>(bits) + static_cast<std::size_t>(width) * height * 4);
    SelectObject(memory, previous);
    DeleteDC(memory);
    DeleteObject(target);
    return result;
}

using FrameInfoFunction = HCURSOR(WINAPI*)(HCURSOR, DWORD, DWORD, DWORD*, DWORD*);

FrameInfoFunction FrameInfo() {
    return reinterpret_cast<FrameInfoFunction>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetCursorFrameInfo")));
}

bool Near(int value, int expected, int tolerance) { return std::abs(value - expected) <= tolerance; }

// Opacity, shadow and colors apply to every role, keep straight alpha (the
// representation Windows draws) and leave animated roles animated.
void TestCursorLook(clickshade::CursorEngine& engine, bool wine) {
    std::wstring error;
    Require(engine.Restore(error) && engine.Configure({}, error), "cursor look tests begin from the current scheme");
    const auto plain = Preview(engine, false);
    if (plain.color.bgra.empty()) {
        Skip("cursor look pixel checks: the current arrow has no color plane");
        return;
    }
    const LONG width = plain.color.width, height = plain.color.height;
    const auto pixelAt = [](const CursorSnapshot& image, LONG x, LONG y) {
        return image.color.bgra.data() + (static_cast<std::size_t>(y) * image.color.width + x) * 4;
    };

    clickshade::CursorAppearance faded; faded.opacity = 50;
    Require(engine.Configure(faded, error), "a 50 percent cursor opacity configures");
    const auto half = Preview(engine, false);
    bool sameSize = half.color.width == width && half.color.height == height;
    bool alphaHalved = sameSize, colorKept = sameSize, renderedHalf = sameSize;
    int opaque = 0, white = 0;
    if (sameSize) {
        OwnedCursor handle(engine.CopyPreview(false));
        const auto onBlack = Rendered(handle.value, width, height, 0);
        for (LONG y = 0; y < height; ++y) for (LONG x = 0; x < width; ++x) {
            const auto* source = pixelAt(plain, x, y);
            const auto* result = pixelAt(half, x, y);
            if (source[3] != 255) continue;
            ++opaque;
            alphaHalved &= Near(result[3], 128, 1);
            for (int c = 0; c < 3; ++c) colorKept &= Near(result[c], source[c], 2);
            if (source[0] == 255 && source[1] == 255 && source[2] == 255) {
                ++white;
                const auto* drawn = onBlack.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                for (int c = 0; c < 3; ++c) renderedHalf &= Near(drawn[c], 128, 3);
            }
        }
    }
    Check(sameSize && opaque > 0 && alphaHalved, "50 percent opacity halves the alpha of every opaque pixel");
    Check(colorKept, "opacity keeps straight cursor colors instead of darkening them");
    if (white) Check(renderedHalf, "a faded white pixel draws as half-bright grey over black, not quarter-bright");
    else Skip("faded white rendering: the current arrow has no opaque white pixel");
    TestPreviews(engine);
    clickshade::CursorAppearance tooFaint; tooFaint.opacity = 29;
    Check(!engine.Configure(tooFaint, error) && !error.empty(), "an opacity below 30 percent is rejected");

    clickshade::CursorAppearance shadowed; shadowed.shadow = true;
    Require(engine.Configure(shadowed, error), "a cursor shadow configures");
    const auto soft = Preview(engine, false), softPressed = Preview(engine, true);
    const LONG pad = static_cast<LONG>(soft.xHotspot) - static_cast<LONG>(plain.xHotspot);
    Check(soft.color.width > width && soft.color.height > height && pad > 0 &&
          static_cast<LONG>(soft.yHotspot) - static_cast<LONG>(plain.yHotspot) == pad,
          "the shadow grows the canvas and moves the hotspot with the artwork");
    bool artKept = true, shadowDark = true;
    int shadowPixels = 0;
    for (LONG y = 0; y < soft.color.height; ++y) for (LONG x = 0; x < soft.color.width; ++x) {
        const auto* result = pixelAt(soft, x, y);
        const auto* pressed = pixelAt(softPressed, x, y);
        const LONG sx = x - pad, sy = y - pad;
        const bool inside = sx >= 0 && sy >= 0 && sx < width && sy < height;
        const auto* source = inside ? pixelAt(plain, sx, sy) : nullptr;
        if (source && source[3] == 255) {
            for (int c = 0; c < 4; ++c) artKept &= result[c] == source[c];
        } else if ((!source || !source[3]) && result[3]) {
            ++shadowPixels;
            shadowDark &= result[3] <= 100 && !result[0] && !result[1] && !result[2] &&
                          !pressed[0] && !pressed[1] && !pressed[2];
        }
    }
    Check(artKept, "the shadow leaves opaque artwork pixels unchanged");
    Check(shadowPixels > 0 && shadowDark, "the shadow is a soft dark band that stays dark on the pressed cursor");
    TestPreviews(engine);

    clickshade::CursorAppearance tinted; tinted.tint = true;
    tinted.fillColor = RGB(255, 0, 0); tinted.outlineColor = RGB(0, 0, 255);
    Require(engine.Configure(tinted, error), "custom cursor colors configure");
    const auto colored = Preview(engine, false), coloredPressed = Preview(engine, true);
    // Light artwork takes the fill color and dark artwork the outline color,
    // blended by brightness, so antialiased outlines stay smooth. Here the
    // fill is red and the outline blue; the pressed cursor is inverted first.
    bool mapped = colored.color.width == width && colored.color.height == height, swapped = mapped;
    int fills = 0, outlines = 0;
    for (LONG y = 0; mapped && y < height; ++y) for (LONG x = 0; x < width; ++x) {
        const auto* source = pixelAt(plain, x, y);
        if (source[3] < 128) continue;
        const double light = (0.114 * source[0] + 0.587 * source[1] + 0.299 * source[2]) / 255.0;
        const int red = static_cast<int>(std::lround(255 * light)), blue = 255 - red;
        const auto* released = pixelAt(colored, x, y);
        const auto* pressed = pixelAt(coloredPressed, x, y);
        fills += light > 0.9; outlines += light < 0.25;
        mapped &= Near(released[2], red, 4) && released[1] <= 2 && Near(released[0], blue, 4);
        swapped &= Near(pressed[2], blue, 4) && pressed[1] <= 2 && Near(pressed[0], red, 4);
    }
    Check(fills > 0 && outlines > 0, "the arrow has both light fill and dark outline artwork to recolor");
    Check(mapped, "light artwork takes the fill color and dark artwork the outline color");
    Check(swapped, "the pressed cursor swaps fill and outline colors");

    clickshade::CursorAppearance combined = tinted; combined.opacity = 60; combined.shadow = true; combined.scale = 200;
    Require(engine.Configure(combined, error), "scale, opacity, shadow and colors configure together");
    TestPreviews(engine);
    Require(engine.SetActive(true, error) && engine.SetPressed(true, error) && engine.SetPressed(false, error),
            "a fully styled profile activates and handles a click");
    Require(engine.Restore(error), "a styled profile restores the original scheme");

    const auto frameInfo = FrameInfo();
    if (wine || !frameInfo) {
        Skip("animated cursor look: needs native Windows and GetCursorFrameInfo");
        return;
    }
    DWORD rate = 0, frames = 0;
    const HCURSOR systemWait = LoadCursorW(nullptr, IDC_WAIT);
    const HCURSOR firstFrame = frameInfo(systemWait, 0, 0, &rate, &frames);
    // Every earlier profile, pause and reconfiguration ran before this point,
    // so a registered .ani Wait cursor must still be animated here.
    wchar_t registered[MAX_PATH]{};
    DWORD bytes = sizeof(registered);
    const bool aniRegistered = RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"Wait",
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, registered, &bytes) == ERROR_SUCCESS &&
        std::wcslen(registered) > 4 && _wcsicmp(registered + std::wcslen(registered) - 4, L".ani") == 0;
    if (aniRegistered) Check(frames > 1 && firstFrame, "earlier profiles and pauses leave the animated Wait cursor animated");
    if (frames <= 1 || !firstFrame) {
        Skip("animated cursor look: the current Wait cursor is not animated");
        return;
    }
    const auto originalFrame = ReadCursor(firstFrame);
    Require(engine.Configure(faded, error) && engine.SetActive(true, error), "a faded profile activates over an animated Wait cursor");
    DWORD styledRate = 0, styledFrames = 0;
    const HCURSOR styledFirst = frameInfo(LoadCursorW(nullptr, IDC_WAIT), 0, 0, &styledRate, &styledFrames);
    Check(styledFrames == frames && styledRate == rate, "the faded Wait cursor keeps every animation frame and its timing");
    if (styledFirst) {
        const auto styledFrame = ReadCursor(styledFirst);
        bool frameHalved = styledFrame.color.width == originalFrame.color.width &&
                           styledFrame.color.height == originalFrame.color.height && !originalFrame.color.bgra.empty();
        bool frameColors = frameHalved;
        int counted = 0;
        for (std::size_t i = 0; frameHalved && i + 3 < originalFrame.color.bgra.size(); i += 4) {
            if (originalFrame.color.bgra[i + 3] != 255) continue;
            ++counted;
            frameHalved &= Near(styledFrame.color.bgra[i + 3], 128, 1);
            for (int c = 0; c < 3; ++c) frameColors &= Near(styledFrame.color.bgra[i + c], originalFrame.color.bgra[i + c], 3);
        }
        Check(counted > 0 && frameHalved, "every animated frame is faded to half opacity");
        Check(frameColors, "animated frames keep straight colors through the rebuilt cursor file");
    } else {
        Check(false, "the faded Wait cursor exposes its first frame");
    }
    Require(engine.Restore(error), "the animated look restores the original Wait cursor");
    DWORD restoredFrames = 0;
    frameInfo(LoadCursorW(nullptr, IDC_WAIT), 0, 0, &rate, &restoredFrames);
    Check(restoredFrames == frames, "restoration brings back the original Wait animation");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || std::wcscmp(argv[1], L"--run") != 0) {
        std::puts("Usage: cursor_smoke.exe --run\n"
                  "Close ClickShade first. Run in an interactive Windows test desktop.\n"
                  "This test temporarily changes cursor images, then restores them.");
        return 2;
    }
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ClickShade.Instance.v1");
    if (running) {
        CloseHandle(running);
        std::puts("Close the running ClickShade app before testing its cursor engine.");
        return 2;
    }
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    if (wine) std::puts("Platform: Wine. Shared-system cursor mutation checks will be skipped.");
    else std::puts("Platform: native Windows.");
    try {
        TestPixelConversion();
        TestWindow window;
        const auto baseline = ReadCursor(LoadCursorW(nullptr, IDC_ARROW));
        {
            clickshade::CursorEngine engine;
            std::wstring error;
            Require(engine.Initialize(error), "engine initializes from the active cursor scheme");
            Require(engine.CursorCount() > 0, "engine caches standard cursor roles");
            TestPreviews(engine);
            Require(engine.SetPressed(true, error) && engine.ApplyCurrent(error),
                    "press and apply succeed");
            Check(engine.IsPressed(), "engine reports pressed state");
            if (wine) {
                Skip("system cursor image changes on press: Wine 9 SetSystemCursor is not implemented");
            } else {
                Check(!(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == baseline),
                      "native system arrow image actually changes on press");
            }
            Require(engine.SetPressed(false, error), "release succeeds");
            Check(!engine.IsPressed(), "engine reports released state");
            if (!wine) {
                Check(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == baseline,
                      "native system arrow is restored byte-for-byte on release");
            }
            const DWORD beforeGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            const DWORD beforeUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
            bool cyclesOk = true;
            for (int cycle = 0; cycle < 128; ++cycle) {
                cyclesOk &= engine.SetPressed(true, error) && engine.ApplyCurrent(error);
                cyclesOk &= engine.SetPressed(false, error);
                if (!cyclesOk) break;
            }
            Check(cyclesOk, "128 press/release cycles succeed");
            const DWORD afterGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            const DWORD afterUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
            if (beforeGdi && beforeUser) {
                Check(afterGdi <= beforeGdi + 2 && afterUser <= beforeUser + 2,
                      "repeated swaps do not accumulate GDI or USER handles");
            } else {
                Skip("GUI resource counts unavailable in this runtime");
            }
            Require(engine.Restore(error), "explicit restoration succeeds");
            TestProfiles(engine, baseline, wine);
            TestMultiImageAndCompanions(engine);
            TestLegacyAndClassicScaling(engine);
            TestRegisteredResolutionSource(engine);
            TestMagnification(engine, wine);
            TestHeldRoleTransitions(engine, wine);
            TestCursorLook(engine, wine);
            Require(engine.Reload(error), "cursor cache can reload after restoration");
            TestPreviews(engine);
            Require(engine.SetPressed(true, error) && engine.ApplyCurrent(error),
                    "pressed state is prepared for destructor restoration test");
        }
        if (wine) {
            Skip("system cursor restoration after destruction: Wine does not mutate it");
        } else {
            Check(ReadCursor(LoadCursorW(nullptr, IDC_ARROW)) == baseline,
                  "destruction restores the original system arrow byte-for-byte");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Test stopped: %s\n", error.what());
        if (!failed) ++failed;
    }
    std::printf("\nResults: %d passed, %d failed, %d skipped.\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
