#pragma once

#include <algorithm>
#include <cmath>

namespace clickshade {

// Pure, in-memory workload pacing. Observe only actual render work, with the
// interval between active frame starts as elapsedSeconds. Waiting/sleeping is
// never work; idle intervals do not earn an immediate up/down transition.
class AdaptiveFramePacer {
public:
    bool Configure(unsigned monitorHz, unsigned manualHz = 0) noexcept {
        monitorHz = std::clamp(monitorHz, 30u, 360u);
        manualHz = manualHz ? std::clamp(manualHz, 30u, 360u) : 0u;
        if (monitorHz == monitorHz_ && manualHz == manualHz_) return false;
        monitorHz_ = monitorHz;
        manualHz_ = manualHz;
        rate_ = manualHz_ ? manualHz_ : monitorHz_;
        ClearHistory();
        return true;
    }

    bool Observe(double workSeconds, double elapsedSeconds, bool active = true) noexcept {
        if (!active) { ClearHistory(); return false; }
        if (manualHz_ || !std::isfinite(workSeconds) || !std::isfinite(elapsedSeconds) ||
            workSeconds < 0.0 || elapsedSeconds <= 0.0) return false;
        // A compositor stall is a sample, not permission to spend several
        // seconds of hysteresis at once. Very long gaps start a new observation.
        if (elapsedSeconds > 0.25) ClearHistory();
        const double elapsed = std::clamp(elapsedSeconds, 0.001, 0.05);
        const double work = std::min(workSeconds, 0.25);
        if (!haveAverage_) { averageWork_ = work; haveAverage_ = true; }
        else {
            const double weight = elapsed / (0.15 + elapsed);
            averageWork_ += weight * (work - averageWork_);
        }

        const unsigned previous = rate_;
        if (averageWork_ * rate_ > 0.65) {
            busySeconds_ += elapsed;
            headroomSeconds_ = 0.0;
            if (busySeconds_ >= 0.25 && rate_ > 30) {
                // Lower at most one quarter per observation window. Leave
                // roughly half a frame for input, composition and other apps.
                const unsigned desired = averageWork_ > 0.0
                    ? static_cast<unsigned>(std::clamp(0.50 / averageWork_, 30.0, 360.0)) : monitorHz_;
                rate_ = std::clamp(desired, std::max(30u, rate_ * 3 / 4), rate_ - 1);
                busySeconds_ = 0.0;
            }
        } else {
            busySeconds_ = 0.0;
            const unsigned candidate = std::min(monitorHz_, std::max(rate_ + 1, rate_ * 11 / 10));
            if (rate_ < monitorHz_ && averageWork_ * candidate <= 0.50) {
                headroomSeconds_ += elapsed;
                if (headroomSeconds_ >= 1.25) {
                    rate_ = candidate;
                    headroomSeconds_ = 0.0;
                }
            } else headroomSeconds_ = 0.0;
        }
        return previous != rate_;
    }

    unsigned Rate() const noexcept { return rate_; }
    unsigned MonitorCap() const noexcept { return monitorHz_; }
    bool Automatic() const noexcept { return manualHz_ == 0; }

private:
    void ClearHistory() noexcept {
        haveAverage_ = false;
        averageWork_ = busySeconds_ = headroomSeconds_ = 0.0;
    }
    unsigned monitorHz_ = 60;
    unsigned manualHz_ = 0;
    unsigned rate_ = 60;
    bool haveAverage_ = false;
    double averageWork_ = 0.0;
    double busySeconds_ = 0.0;
    double headroomSeconds_ = 0.0;
};

}  // namespace clickshade
