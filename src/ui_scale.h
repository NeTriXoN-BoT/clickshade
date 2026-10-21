#pragma once

#include <algorithm>
#include <cstdint>

namespace clickshade::ui {

struct WorkArea { int left, top, right, bottom; };
struct Frame { int x, y, width, height; };

inline int Scale(int value, int dpi) noexcept {
    const std::int64_t product = static_cast<std::int64_t>(value) * dpi;
    return static_cast<int>((product + (product >= 0 ? 48 : -48)) / 96);
}

inline int HorizontalMargin(const WorkArea& work) noexcept {
    return std::min(12, std::max(0, (work.right - work.left - 1) / 2));
}
inline int VerticalMargin(const WorkArea& work) noexcept {
    return std::min(16, std::max(0, (work.bottom - work.top - 1) / 2));
}

// Extra UI zoom is constrained by the real monitor work area. A high Windows
// DPI on a small display can legitimately need less than 75% extra zoom to
// keep the minimum usable window on screen. Effect sizes use their own DPI.
inline int EffectiveDpi(int monitorDpi, int requestedZoom, const WorkArea& work) noexcept {
    monitorDpi = std::max(1, monitorDpi);
    const int width = std::max(1, work.right - work.left - 2 * HorizontalMargin(work));
    const int height = std::max(1, work.bottom - work.top - 2 * VerticalMargin(work));
    const auto logicalHeight = static_cast<std::int64_t>(work.bottom - work.top) * 96 / monitorDpi;
    const int zoom = requestedZoom > 0 ? requestedZoom : logicalHeight >= 1800 ? 150 : logicalHeight >= 1280 ? 115 : 100;
    const auto desired = (static_cast<std::int64_t>(monitorDpi) * zoom + 50) / 100;
    const auto fit = std::min(static_cast<std::int64_t>(width) * 96 / 640,
                             static_cast<std::int64_t>(height) * 96 / 520);
    return static_cast<int>(std::max<std::int64_t>(1, std::min(desired, fit)));
}

inline Frame FitFrame(const WorkArea& work, Frame preferred, int dpi) noexcept {
    const int marginX = HorizontalMargin(work), marginY = VerticalMargin(work);
    const int availableWidth = std::max(1, work.right - work.left - 2 * marginX);
    const int availableHeight = std::max(1, work.bottom - work.top - 2 * marginY);
    const int minimumWidth = std::clamp(Scale(640, dpi), 1, availableWidth);
    const int minimumHeight = std::clamp(Scale(520, dpi), 1, availableHeight);
    preferred.width = std::clamp(preferred.width, minimumWidth, availableWidth);
    preferred.height = std::clamp(preferred.height, minimumHeight, availableHeight);
    const int minimumX = work.left + marginX, minimumY = work.top + marginY;
    // The size is already bounded by the available area, so these intervals
    // are ordered even when a resolution change leaves an old window outside.
    const int maximumX = std::max(minimumX, work.right - marginX - preferred.width);
    const int maximumY = std::max(minimumY, work.bottom - marginY - preferred.height);
    preferred.x = std::clamp(preferred.x, minimumX, maximumX);
    preferred.y = std::clamp(preferred.y, minimumY, maximumY);
    return preferred;
}

// The preferred size is in DIP: a full sidebar plus two comfortable card
// columns, and a height the caller refines once the pages are measured.
inline Frame InitialFrame(const WorkArea& work, int actualMonitorDpi, int requestedZoom,
                          int widthDip = 1100, int heightDip = 720) noexcept {
    const int dpi = EffectiveDpi(actualMonitorDpi, requestedZoom, work);
    Frame frame{0, 0, Scale(widthDip, dpi),
        std::min(Scale(heightDip, dpi), std::max(1, work.bottom - work.top - 60))};
    frame.x = work.left + (work.right - work.left - frame.width) / 2;
    frame.y = work.top + (work.bottom - work.top - frame.height) / 2;
    return FitFrame(work, frame, dpi);
}

} // namespace clickshade::ui
