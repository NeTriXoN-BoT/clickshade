// Headless checks of the pointer lead (src/pointer_lead.h) against simulated
// mice (125 to 1000 Hz reports, with jitter) and displays (60 to 360 Hz). The
// frame timing mirrors the renderer: a motion frame runs at the first motion
// deadline after new input, and a settle check runs when the lead would
// expire without new input.
#include "pointer_lead.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace {
int passed = 0, failed = 0;

void Check(bool condition, const char* label, const char* detail = "") {
    std::printf("%s %s%s%s\n", condition ? "PASS" : "FAIL", label, detail[0] ? " | " : "", detail);
    condition ? ++passed : ++failed;
}

struct Point { double x = 0.0, y = 0.0; };
struct Frame { double t = 0.0; Point sampled, drawn; double lead = 0.0; bool settle = false; };
using Path = std::function<Point(double)>;

double AdvanceDeadline(double previous, double now, double interval) {
    return previous > 0.0 && now >= previous && now - previous < interval ? previous + interval : now + interval;
}

// Deterministic jitter of about +-10% on every report interval.
struct Jitter {
    unsigned state = 12345u;
    explicit Jitter(unsigned seed) : state(seed) {}
    double Next() { state = state * 1103515245u + 12345u; return ((state >> 8) % 2001) / 10000.0 - 0.1; }
};

std::vector<Frame> Simulate(double mouseHz, double displayHz, double duration, const Path& path, double scale = 1.0,
                            unsigned seed = 12345u) {
    clickshade::PointerLead lead;
    const double frame = 1.0 / displayHz, report = 1.0 / mouseHz;
    lead.Configure(frame, scale);
    Jitter jitter(seed);
    double nextReport = 0.0, nextMotion = 0.0;
    bool pending = false;
    Point exact = path(0.0), reported{std::round(exact.x), std::round(exact.y)};
    std::vector<Frame> frames;
    for (double t = 0.0; t < duration; t += 0.0001) {
        if (t >= nextReport) {
            const Point now = path(t);
            if (now.x != exact.x || now.y != exact.y) pending = true;
            exact = now;
            reported = {std::round(now.x), std::round(now.y)};
            nextReport += report * (1.0 + jitter.Next());
        }
        if (pending && t >= nextMotion) {
            const auto offset = lead.Update(t, reported.x, reported.y);
            frames.push_back({t, reported, {reported.x + std::round(offset.x), reported.y + std::round(offset.y)},
                              std::hypot(offset.x, offset.y), false});
            pending = false;
            nextMotion = AdvanceDeadline(nextMotion, t, frame);
        } else if (!pending && lead.Active() && t >= lead.SettleTime()) {
            lead.Settle(t);
            const auto offset = lead.Current();
            frames.push_back({t, reported, {reported.x + std::round(offset.x), reported.y + std::round(offset.y)},
                              std::hypot(offset.x, offset.y), true});
        }
    }
    return frames;
}

Path Line(double speed, double stopAt = 1e9, double brakeSeconds = 0.0) {
    const double dx = 1.0 / std::hypot(1.0, 0.3), dy = 0.3 / std::hypot(1.0, 0.3);
    return [=](double t) {
        double distance;
        if (t <= stopAt) distance = speed * t;
        else if (t <= stopAt + brakeSeconds) {
            const double s = t - stopAt;
            distance = speed * stopAt + speed * (s - s * s / (2.0 * brakeSeconds));
        } else distance = speed * stopAt + speed * brakeSeconds / 2.0;
        return Point{100.0 + dx * distance, 100.0 + dy * distance};
    };
}

double Along(const Point& p) {
    const double dx = 1.0 / std::hypot(1.0, 0.3), dy = 0.3 / std::hypot(1.0, 0.3);
    return (p.x - 100.0) * dx + (p.y - 100.0) * dy;
}

double Median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

const double kMice[] = {125.0, 250.0, 500.0, 1000.0};
const double kDisplays[] = {60.0, 144.0, 165.0, 240.0, 360.0};
const unsigned kSeeds[] = {12345u, 777u, 424242u};

void SteadyLines() {
    int backward = 0, flicker = 0, missingLead = 0;
    char detail[256]{};
    for (const unsigned seed : kSeeds) for (const double mouse : kMice) for (const double display : kDisplays) {
        const auto frames = Simulate(mouse, display, 0.8, Line(2400.0), 1.0, seed);
        std::vector<double> leads;
        double previous = -1e9;
        int comboBackward = 0, comboFlicker = 0;
        for (const auto& frame : frames) {
            if (frame.t < 0.15) { previous = Along(frame.drawn); continue; }
            const double along = Along(frame.drawn);
            if (along < previous - 0.75) ++comboBackward;
            previous = along;
            leads.push_back(frame.lead);
        }
        const double median = Median(leads);
        for (const double value : leads) if (median >= 1.5 && value < 0.5 * median) ++comboFlicker;
        clickshade::PointerLead probe; probe.Configure(1.0 / display, 1.0);
        const double expected = 2400.0 * probe.LeadSeconds();
        if (median < 0.5 * expected) {
            ++missingLead;
            std::snprintf(detail, sizeof(detail), "%.0f Hz mouse, %.0f Hz display: median lead %.1f px, expected %.1f", mouse, display, median, expected);
        }
        if (comboBackward || comboFlicker)
            std::printf("  %.0f Hz mouse, %.0f Hz display: %d backward, %d flicker, median lead %.1f px\n",
                        mouse, display, comboBackward, comboFlicker, median);
        backward += comboBackward; flicker += comboFlicker;
    }
    Check(backward == 0, "steady fast motion: the drawn position never steps back (all mice, displays and timings)");
    Check(flicker == 0, "steady fast motion: the lead never drops out and returns (all mice, displays and timings)");
    Check(missingLead == 0, "steady fast motion: about half a frame of lead is applied (all mice and displays)", detail);
}

void SlowMotion() {
    bool none = true;
    for (const double mouse : kMice) for (const double display : kDisplays)
        for (const auto& frame : Simulate(mouse, display, 0.6, Line(400.0))) none &= frame.lead == 0.0;
    Check(none, "slow, precise motion (400 px/s) is never shifted");
}

void AbruptStops() {
    bool bounded = true, settled = true;
    char detail[256]{};
    for (const unsigned seed : kSeeds) for (const double mouse : kMice) for (const double display : kDisplays) {
        const double stopAt = 0.4;
        const auto path = Line(3000.0, stopAt);
        const double stop = Along(path(stopAt));
        const auto frames = Simulate(mouse, display, 0.6, path, 1.0, seed);
        clickshade::PointerLead probe; probe.Configure(1.0 / display, 1.0);
        const double allowed = 3000.0 * probe.LeadSeconds() + 1.5;
        double overshoot = 0.0, settledAt = -1.0;
        for (const auto& frame : frames) {
            if (frame.t < stopAt) continue;
            overshoot = std::max(overshoot, Along(frame.drawn) - stop);
            if (frame.lead == 0.0 && settledAt < 0.0) settledAt = frame.t;
            if (frame.lead != 0.0) settledAt = -1.0;
        }
        const double limit = stopAt + 0.06 + 2.0 / display + 2.0 / mouse;
        if (overshoot > allowed) {
            bounded = false;
            std::snprintf(detail, sizeof(detail), "%.0f Hz mouse, %.0f Hz display: %.1f px past the stop, allowed %.1f", mouse, display, overshoot, allowed);
        }
        if (settledAt < 0.0 || settledAt > limit || frames.empty() || frames.back().lead != 0.0 ||
            Along(frames.back().drawn) != Along(frames.back().sampled)) {
            settled = false;
            std::snprintf(detail, sizeof(detail), "%.0f Hz mouse, %.0f Hz display: settled at %.1f ms after the stop",
                          mouse, display, (settledAt - stopAt) * 1000.0);
        }
    }
    Check(bounded, "an abrupt stop overshoots by at most the lead itself", detail);
    Check(settled, "after a stop the effects settle exactly on the pointer within one report gap", detail);
}

void Braking() {
    double worst = 0.0;
    for (const double mouse : kMice) for (const double display : kDisplays) {
        const double brakeAt = 0.35;
        const auto path = Line(3000.0, brakeAt, 0.12);
        const double stop = Along(path(1.0));
        for (const auto& frame : Simulate(mouse, display, 0.7, path))
            if (frame.t >= brakeAt) worst = std::max(worst, Along(frame.drawn) - stop);
    }
    char detail[64]; std::snprintf(detail, sizeof(detail), "worst %.1f px past the resting point", worst);
    Check(worst <= 1.5, "a natural 120 ms braking never carries the effects past where the pointer comes to rest", detail);
}

void Circles() {
    int backward = 0;
    for (const double mouse : {500.0, 1000.0}) for (const double display : kDisplays) {
        const double radius = 260.0, omega = 3000.0 / radius;
        const auto frames = Simulate(mouse, display, 0.8, [=](double t) {
            return Point{600.0 + radius * std::cos(omega * t), 400.0 + radius * std::sin(omega * t)};
        });
        double previous = -1e9, unwrap = 0.0, last = 0.0;
        for (const auto& frame : frames) {
            double angle = std::atan2(frame.drawn.y - 400.0, frame.drawn.x - 600.0);
            if (frame.t > 0.0 && angle - last < -3.0) unwrap += 2.0 * 3.14159265358979323846;
            last = angle;
            const double total = angle + unwrap;
            if (frame.t > 0.15 && total < previous - 0.004) ++backward;
            previous = total;
        }
    }
    Check(backward == 0, "fast circles: the drawn position always moves forward");
}

void Reversal() {
    double worst = 0.0;
    for (const double mouse : kMice) for (const double display : kDisplays) {
        // Out at 3000 px/s for 0.3 s, then straight back.
        const auto frames = Simulate(mouse, display, 0.6, [](double t) {
            const double distance = t < 0.3 ? 3000.0 * t : 900.0 - 3000.0 * (t - 0.3);
            return Point{100.0 + distance, 300.0};
        });
        for (const auto& frame : frames) if (frame.t >= 0.3) worst = std::max(worst, frame.drawn.x - 1000.0);
    }
    char detail[64]; std::snprintf(detail, sizeof(detail), "worst %.1f px beyond the turning point", worst);
    clickshade::PointerLead probe; probe.Configure(1.0 / 60.0, 1.0);
    Check(worst <= 3000.0 * probe.LeadSeconds() + 1.5, "a sharp reversal overshoots by at most the lead", detail);
}

void Scale() {
    // At 200% the same physical speed counts as half as fast: 1200 px/s on a
    // 192 DPI monitor is 600 px/s at 96 DPI, below the lead threshold.
    bool none = true;
    for (const auto& frame : Simulate(1000.0, 240.0, 0.5, Line(1200.0), 2.0)) none &= frame.lead == 0.0;
    Check(none, "speed thresholds follow the monitor scale");
}

void Stalls() {
    // A renderer that misses several frames during fast motion (a busy
    // system): the next samples cannot be fitted yet. The lead fades instead
    // of becoming invalid, and the drawn position never steps back far.
    bool finite = true;
    double worstBack = 0.0;
    for (const double display : {144.0, 240.0, 360.0}) {
        clickshade::PointerLead lead;
        lead.Configure(1.0 / display, 1.0);
        double previous = -1e9;
        for (double t = 0.0; t < 0.6; t += 1.0 / display) {
            const double stalled = t >= 0.3 && t < 0.3 + 0.02 ? 0.3 : t; // 20 ms without frames
            if (stalled != t) continue;
            const double x = std::round(2400.0 * t);
            const auto offset = lead.Update(t, x, 100.0);
            finite &= std::isfinite(offset.x) && std::isfinite(offset.y);
            const double drawn = x + std::round(offset.x);
            if (t > 0.15) worstBack = std::max(worstBack, previous - drawn);
            previous = drawn;
        }
    }
    char detail[64]; std::snprintf(detail, sizeof(detail), "worst step back %.1f px", worstBack);
    Check(finite && worstBack <= 0.75, "a stalled renderer fades the lead instead of breaking it", detail);
}

void Pauses() {
    // Moving, a 30 ms pause, moving again: the history restarts cleanly.
    bool clean = true;
    const auto frames = Simulate(1000.0, 240.0, 0.6, [](double t) {
        const double distance = t < 0.25 ? 2400.0 * t : t < 0.28 ? 600.0 : 600.0 + 2400.0 * (t - 0.28);
        return Point{100.0 + distance, 100.0};
    });
    clickshade::PointerLead probe; probe.Configure(1.0 / 240.0, 1.0);
    const double full = 2400.0 * probe.LeadSeconds();
    bool settled = false;
    for (const auto& frame : frames) {
        if (frame.t >= 0.25 && frame.t < 0.28) settled |= frame.lead == 0.0;
        // The first frames after the pause carry at most half the lead.
        if (frame.t >= 0.28 && frame.t < 0.28 + 2.5 / 240.0 && frame.lead > 0.5 * full) clean = false;
    }
    Check(settled && clean, "after a short pause the lead builds up again instead of jumping ahead");
}
} // namespace

int wmain() {
    SteadyLines();
    SlowMotion();
    AbruptStops();
    Braking();
    Circles();
    Reversal();
    Scale();
    Stalls();
    Pauses();
    std::printf("RESULT %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
