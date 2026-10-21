#include "ui_scale.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned passed = 0;
void Check(bool condition, const char* description) {
    if (!condition) { std::printf("FAIL %s\n", description); std::exit(1); }
    ++passed;
    std::printf("PASS %s\n", description);
}
bool Inside(const clickshade::ui::WorkArea& work, const clickshade::ui::Frame& frame) {
    return frame.width > 0 && frame.height > 0 && frame.x >= work.left && frame.y >= work.top &&
        frame.x + frame.width <= work.right && frame.y + frame.height <= work.bottom;
}
}

int main() {
    using namespace clickshade::ui;
    const WorkArea small{0, 0, 1280, 720};
    const int smallDpi = EffectiveDpi(192, 0, small);
    Check(smallDpi < 144 && Scale(640, smallDpi) <= 1256 && Scale(520, smallDpi) <= 688,
          "1280x720 at 200% Windows DPI reduces extra zoom enough to fit");
    Check(Inside(small, FitFrame(small, {-900, 1200, 2400, 1800}, smallDpi)),
          "an oversized offscreen window fits after a high-DPI resolution change");

    const WorkArea secondary{1920, 0, 5760, 2160};
    const auto actual = InitialFrame(secondary, 192, 100);
    const auto primaryAssumption = InitialFrame(secondary, 96, 100);
    Check(actual.width == 2200 && primaryAssumption.width == 1100 && Inside(secondary, actual),
          "secondary-monitor startup dimensions use actual window DPI, not primary DPI");
    const auto measured = InitialFrame(secondary, 96, 100, 1100, 1500);
    const auto oversized = InitialFrame(secondary, 96, 100, 1100, 3000);
    Check(measured.height == 1500 && oversized.height == 2100 && Inside(secondary, oversized),
          "a measured page height is honored up to the work area with a 60 px reserve");
    const WorkArea leftMonitor{-1920, -200, 0, 880};
    Check(Inside(leftMonitor, InitialFrame(leftMonitor, 144, 150)),
          "negative monitor coordinates remain valid when centering and fitting");
    const auto compact = FitFrame({0, 0, 1920, 1080}, {20, 20, 640, 540}, 96);
    Check(compact.width == 640 && compact.height == 540,
          "the responsive two-row navigation allows a usable 640-DIP window");
    const WorkArea tiny{30, 40, 50, 60};
    Check(Inside(tiny, InitialFrame(tiny, 480, 200)),
          "tiny work areas never create reversed clamp intervals or nonpositive extents");

    unsigned cases = 0;
    for (const auto area : std::array<WorkArea, 6>{{
            {0, 0, 720, 480}, {0, 0, 1280, 720}, {-1920, 0, 0, 1040},
            {0, 0, 2560, 1400}, {1920, 0, 5760, 2120}, {-7680, -2160, 0, 2160}}}) {
        for (int dpi : {96, 120, 144, 192, 240, 288, 384, 480}) {
            for (int zoom : {0, 100, 115, 150, 200}) {
                const int effective = EffectiveDpi(dpi, zoom, area);
                const auto frame = InitialFrame(area, dpi, zoom);
                const auto displaced = FitFrame(area, {100000, -100000, 100000, 100000}, effective);
                if (!Inside(area, frame) || !Inside(area, displaced)) {
                    std::printf("FAIL fit invariant: dpi=%d zoom=%d\n", dpi, zoom);
                    return 1;
                }
                ++cases;
            }
        }
    }
    Check(cases == 240, "240 resolution, DPI, zoom and virtual-desktop combinations preserve fit invariants");
    std::printf("RESULT %u passed; portable geometry only, native mixed-monitor delivery remains unverified.\n", passed);
    return 0;
}
