#pragma once

#include "runtime.h"
#include <memory>
#include <string>

namespace clickshade {

// Read-only cumulative counters. A concurrent snapshot is approximate; these
// describe submitted work, not display presentation or input-to-photon latency.
struct VisualEffectsStats {
    std::uint64_t frameCount = 0;
    std::uint64_t highlightMoves = 0;
    std::uint64_t highlightRasterizations = 0;
    std::uint64_t clickFrames = 0;
    std::uint64_t pointerWakeups = 0;
    std::uint64_t stackWakeups = 0;
    std::uint64_t magnifierUpdates = 0; // Lens source refreshes (moves and live redraws).
    std::uint64_t keyDisplayUpdates = 0; // Key panel repaints caused by presses.
    unsigned targetRefreshRate = 60;
    unsigned motionRefreshRate = 60;
    unsigned monitorDpi = 96;
    bool highResolutionTimer = false;
    bool framePending = false;
    bool magnifierVisible = false;
    bool keyDisplayVisible = false;
};

// Owns a small set of native, click-through layered windows on a separate
// message-driven rendering thread. High-rate input notifications never wait
// for rendering and are coalesced while a movement frame is scheduled.
class VisualEffects {
public:
    VisualEffects();
    ~VisualEffects();
    VisualEffects(const VisualEffects&) = delete;
    VisualEffects& operator=(const VisualEffects&) = delete;

    bool Start(const Settings& settings, std::wstring& error);
    void Stop();
    void UpdateSettings(const Settings& settings);
    void SetAllowed(bool allowed);
    // keyDisplayAllowed may stay true while a hidden pointer pauses the rest.
    void SetAllowed(bool allowed, bool keyDisplayAllowed);
    void PointerActivity() noexcept;
    void Click(unsigned buttonMask) noexcept;
    // One key (or mouse button, with kModMouse) for the on-screen key display.
    // The identity only travels to the renderer to become a label; it is
    // dropped unless the key display is on and allowed.
    void Key(unsigned virtualKey, unsigned scanCode, bool extended, unsigned modifiers) noexcept;
    // Shows a sample label even while the key display switch is off.
    void PreviewKey() noexcept;
    void SetNotifyWindow(HWND window, UINT message) noexcept;
    std::wstring LastError() const;
    VisualEffectsStats Stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clickshade
