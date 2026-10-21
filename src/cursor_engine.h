#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace clickshade {

// Inverts straight-alpha BGRA cursor pixels in place, preserving alpha.
// Windows stores and draws every cursor color bitmap with straight alpha,
// whether it came from a file or from CreateIconIndirect. Fully transparent
// pixels (including their hidden RGB data) remain unchanged.
void InvertCursorBgra(std::uint8_t* pixels, std::size_t count) noexcept;

// Looks only beside an explicitly selected .cur file. Recognized final words
// are normal/released <-> pressed, up <-> down, white <-> black and light <->
// dark. Returns false, with cleared outputs, when there is no unique partner.
// This helper does file metadata work and belongs on the settings/file path.
bool FindCursorCompanion(const std::wstring& selected, std::wstring& companion,
                         bool& selectedIsPressed);

struct CursorAppearance {
    // 0: installed Windows scheme; 1: installed Windows default assets;
    // 2: local classic at original size, scalable Classic at manual sizes;
    // 3: custom static .cur.
    int style = 0;
    bool reverse = false;
    // Zero retains source dimensions (or applies `scale`). Otherwise the
    // longest image dimension is 24..256 physical image pixels; the hotspot
    // scales with the artwork.
    int size = 0;
    // Percent of the source arrow's size, 100..600, used while `size` is
    // zero; every role then gets the same pixel size, at most 256.
    int scale = 100;
    // Look applied to every role, animated ones included: opacity 30..100
    // percent, a soft drop shadow, and optional fill/outline colors that
    // repaint light and dark artwork (the pressed cursor therefore gets them
    // swapped). Genuine background-inverting pixels keep their look.
    int opacity = 100;
    bool shadow = false;
    bool tint = false;
    COLORREF fillColor = RGB(255, 255, 255);
    COLORREF outlineColor = RGB(0, 0, 0);
    std::wstring releasedPath;
    std::wstring pressedPath;  // Empty means automatic color inversion.
    // Optional shake-to-locate cache. One disables it; 1..4 prepares bounded
    // Arrow artwork up front. No file decoding or rasterizing occurs in
    // SetMagnification. Enlarged images are limited to 256 pixels per side.
    float maximumMagnification = 1.f;
};

class CursorEngine {
public:
    CursorEngine();
    ~CursorEngine();
    CursorEngine(const CursorEngine&) = delete;
    CursorEngine& operator=(const CursorEngine&) = delete;
    CursorEngine(CursorEngine&&) = delete;
    CursorEngine& operator=(CursorEngine&&) = delete;

    bool Initialize(std::wstring& error);
    // Configuration prepares all files/artwork outside the click path. On
    // success the original user cursors are restored and the engine is idle.
    bool Configure(const CursorAppearance& appearance, std::wstring& error);
    // Active installs the configured released artwork; inactive restores the
    // original user scheme snapshots, independently of reverse/profile state.
    bool SetActive(bool active, std::wstring& error);
    bool SetPressed(bool pressed, std::wstring& error);
    // Selects a prepared Arrow size for supported visible system cursor roles.
    // One restores their configured shapes; inactive/Restore cancels it.
    bool SetMagnification(float factor, std::wstring& error);
    bool ApplyCurrent(std::wstring& error);
    bool Restore(std::wstring& error) noexcept;
    bool Reload(std::wstring& error);
    bool IsPressed() const;
    std::size_t CursorCount() const;

    // The caller owns the selected released/pressed Arrow artwork and must
    // call DestroyCursor. Reverse and current magnification are reflected.
    HCURSOR CopyPreview(bool pressed) const;
    static bool RestoreSystemScheme() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clickshade
