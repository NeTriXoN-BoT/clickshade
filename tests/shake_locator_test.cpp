#include "../src/shake_locator.h"
#include <cmath>
#include <iostream>
#include <limits>

namespace {
unsigned passed = 0, failed = 0;
void Check(bool condition, const char* name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    condition ? ++passed : ++failed;
}
bool Near(double a, double b) { return std::abs(a - b) < 0.000001; }
bool Shake(clickshade::ShakeLocator& locator, double start, double span = 120.0) {
    bool detected = false;
    for (unsigned i = 0; i < 7; ++i)
        detected = locator.Observe(i & 1u ? span : 0.0, 0.0, start + i * 0.05) || detected;
    return detected;
}
}

int main() {
    using clickshade::ShakeLocator;
    ShakeLocator locator;
    Check(!Shake(locator, 0.0) && Near(locator.Scale(1.0), 1.0), "disabled locator does not enlarge");
    locator.Configure(1);
    bool detected = false;
    for (unsigned i = 0; i < 1000; ++i)
        detected = locator.Observe(i * 2.0, i * 0.5, i / 1000.0) || detected;
    Check(!detected && locator.TraceSize() <= 2, "straight fast movement does not trigger or grow history");
    locator.Reset();
    Check(!Shake(locator, 0.0, 50.0), "small jitter below minimum span is ignored");
    locator.Reset();
    for (unsigned i = 0; i < 5; ++i) detected = locator.Observe(i & 1u ? 120.0 : 0.0, 0.0, i * 0.05);
    Check(!detected && !locator.Active(0.2), "ratio of exactly four does not trigger Plasma preset");
    Check(locator.Observe(120.0, 0.0, 0.25), "ratio above four triggers Plasma preset");
    Check(Near(locator.Scale(0.25), 1.0), "growth starts continuously at original size");
    Check(Near(locator.Scale(0.30), 1.125), "Plasma quarter-transition follows cubic easing");
    Check(Near(locator.Scale(0.35), 2.0), "Plasma 100ms midpoint has half scale interpolation");
    Check(Near(locator.Scale(0.45), 3.0), "Plasma reaches three times after200ms");
    Check(Near(locator.NextWake(0.5, 1.0 / 240.0), 2.25), "hold sleeps until one deadline instead of polling");
    Check(Near(locator.Scale(2.25), 3.0) && Near(locator.Scale(2.35), 2.0), "two-second hold starts a200ms smooth return");
    Check(Near(locator.Scale(2.45), 1.0) && !locator.Active(2.45), "return restores exact original scale");
    Check(std::isinf(locator.NextWake(2.5, 1.0 / 60.0)), "idle locator has no scheduled wake");
    locator.Reset();
    Check(Shake(locator, 0.0) && Shake(locator, 0.5) && Near(locator.Scale(1.0), 4.0), "repeated shake grows by one up to maximum");
    Check(Shake(locator, 1.1) && Near(locator.Scale(1.6), 4.0), "repeated shake respects configured maximum");
    locator.Configure(1, 100, 150);
    Check(Shake(locator, 0.0) && Near(locator.Scale(0.5), 1.5), "user maximum below three is respected");
    locator.Configure(1);
    for (unsigned i = 0; i < 4; ++i) locator.Observe(i & 1u ? 120.0 : 0.0, 0.0, i * 0.05);
    locator.Observe(120.0, 0.0, 0.2, false);
    Check(!locator.Observe(0.0, 0.0, 0.25) && !locator.Active(0.25), "ineligible held-button motion clears accumulated gesture");
    locator.Reset();
    for (unsigned i = 0; i < 4; ++i) locator.Observe(i & 1u ? 120.0 : 0.0, 0.0, i * 0.05);
    Check(!locator.Observe(0.0, 0.0, 2.0) && locator.TraceSize() == 1, "old trace is pruned after detector interval");
    locator.Observe(std::numeric_limits<double>::quiet_NaN(), 0.0, 2.1);
    Check(locator.TraceSize() == 0, "nonfinite position is rejected");
    locator.Observe(0.0, 0.0, 3.0);
    locator.Observe(120.0, 0.0, 2.0);
    Check(locator.TraceSize() == 1, "backward clock discontinuity restarts trace");
    locator.Reset();
    for (unsigned i = 0; i < 10000; ++i) locator.Observe(i & 1u ? 4.0 : 0.0, 0.0, i / 100000.0);
    Check(locator.TraceSize() <= 256 && !locator.Active(1.0), "adversarial jitter keeps bounded trace without false enlargement");
    locator.Configure(1, 200);
    Check(Shake(locator, 0.0, 80.0), "higher user sensitivity accepts a smaller deliberate shake");
    locator.Configure(2);
    Check(Shake(locator, 0.0), "macOS-inspired preset recognizes deliberate shake");
    Check(Near(locator.Scale(0.4), 2.5), "macOS-inspired preset reaches its own documented target");
    Check(Near(locator.Scale(1.3), 1.0), "macOS-inspired preset returns after its own documented hold");
    locator.Configure(2);
    bool quick = false;
    for (unsigned i = 0; i < 5; ++i) quick = locator.Observe(i & 1u ? 65.0 : 0.0, 0.0, 2.0 + i * 0.06) || quick;
    Check(quick, "macOS-inspired preset accepts two quick 65-pixel wiggles");
    locator.Configure(1);
    quick = false;
    for (unsigned i = 0; i < 5; ++i) quick = locator.Observe(i & 1u ? 65.0 : 0.0, 0.0, 2.0 + i * 0.06) || quick;
    Check(!quick, "the same small wiggle stays below the published Plasma span");
    locator.Configure(1);
    Shake(locator, 0.0);
    const double deadline = locator.NextWake(0.26, 1.0 / 240.0);
    Check(Near(deadline, 0.26 + 1.0 / 240.0), "high-refresh animation uses caller deadline");
    const double beforeEnd = 2.44;
    const double terminalWake = locator.NextWake(beforeEnd, 1.0 / 60.0);
    Check(std::isfinite(terminalWake) && terminalWake <= 2.46 && Near(locator.Scale(2.46), 1.0),
          "deadline sampled before a render overrun still requests the terminal restoration frame");
    locator.Reset();
    Check(Near(locator.Scale(0.3), 1.0) && std::isinf(locator.NextWake(0.3, 1.0 / 60.0)), "rule/session reset immediately restores original size and idle");
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
