#include "frame_pacing.h"

#include <cstdio>
#include <limits>

namespace {
int passed = 0, failed = 0;
void Check(bool result, const char* text) {
    std::printf("%s %s\n", result ? "PASS" : "FAIL", text);
    result ? ++passed : ++failed;
}
void Run(clickshade::AdaptiveFramePacer& pacer, double work, double seconds) {
    double elapsed = 0.0;
    while (elapsed < seconds) {
        const double interval = 1.0 / pacer.Rate();
        pacer.Observe(work, interval);
        elapsed += interval;
    }
}
}

int main() {
    clickshade::AdaptiveFramePacer pacer;
    Check(pacer.Rate() == 60 && pacer.Automatic(), "default pacing is automatic 60 Hz");
    Check(pacer.Configure(240) && pacer.Rate() == 240, "Auto begins at the actual monitor cap");
    Run(pacer, 0.0005, 3.0);
    Check(pacer.Rate() == 240, "ample measured headroom retains the monitor cap");
    pacer.Observe(0.200, 1.0 / 240);
    Run(pacer, 0.0005, 0.10);
    Check(pacer.Rate() == 240, "one compositor stall does not immediately lower Auto");
    Run(pacer, 0.009, 3.0);
    const unsigned reduced = pacer.Rate();
    Check(reduced >= 30 && reduced <= 73, "sustained expensive frames lower Auto to a feasible rate");
    Check(!pacer.Configure(240) && pacer.Rate() == reduced, "unchanged configuration preserves learned pacing");
    pacer.Observe(0.0001, 60.0);
    Check(pacer.Rate() == reduced, "an idle gap cannot count as recovery time");
    Run(pacer, 0.0001, 0.50);
    Check(pacer.Rate() == reduced, "recovery requires sustained headroom");
    Run(pacer, 0.0001, 25.0);
    Check(pacer.Rate() == 240, "Auto recovers upward without an idle timer or hardware inventory");
    Run(pacer, 0.100, 8.0);
    Check(pacer.Rate() == 30, "very expensive work respects the 30 Hz floor");
    const unsigned floor = pacer.Rate();
    pacer.Observe(-1.0, 0.01);
    pacer.Observe(0.01, 0.0);
    pacer.Observe(std::numeric_limits<double>::quiet_NaN(), 0.01);
    pacer.Observe(0.01, std::numeric_limits<double>::infinity());
    Check(pacer.Rate() == floor, "invalid measurements do not change pacing");
    pacer.Observe(0.0, 0.02, false);
    Check(pacer.Rate() == floor, "inactive observations preserve the rate without accumulating history");
    Check(pacer.Configure(144) && pacer.Rate() == 144, "moving monitors refreshes the Auto cap");
    Check(pacer.Configure(15) && pacer.Rate() == 30, "unusually low monitor values are safely bounded");
    Check(pacer.Configure(1000) && pacer.Rate() == 360, "unusually high monitor values are safely bounded");
    Check(pacer.Configure(60, 144) && pacer.Rate() == 144 && !pacer.Automatic(),
          "manual pacing retains the explicit requested limit");
    Run(pacer, 0.100, 3.0);
    Check(pacer.Rate() == 144, "measured workload does not silently alter a manual limit");
    pacer.Configure(60, 1);
    Check(pacer.Rate() == 30, "manual pacing accepts and clamps to 30 Hz");
    pacer.Configure(60, 1000);
    Check(pacer.Rate() == 360, "manual pacing has a bounded 360 Hz maximum");
    pacer.Configure(165, 0);
    Check(pacer.Rate() == 165 && pacer.Automatic(), "switching back to Auto uses the current monitor");
    unsigned previous = pacer.Rate();
    bool bounded = true;
    for (unsigned frame = 0; frame < 1000; ++frame) {
        pacer.Observe(0.020, 1.0 / pacer.Rate());
        bounded = bounded && pacer.Rate() >= previous * 3 / 4;
        previous = pacer.Rate();
    }
    Check(bounded, "each downward transition is bounded rather than oscillating abruptly");
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
