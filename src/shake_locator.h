#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace clickshade {

// Independent, bounded implementation of a shake-to-locate gesture. Positions
// are OS cursor coordinates expressed in logical pixels, never raw mouse
// deltas. Call Reset when a desktop/rule/monitor-scale boundary invalidates the
// trace; pass eligible=false for held buttons or a known pointer warp.
//
// Plasma-style defaults follow the published KWin 6.8.1 behavior exactly: a
// one-second trace, 100px span, path/span > 4, 3x then +1x, 200ms InOutCubic
// transitions and a two-second hold. The configurable 4x ceiling is our
// resource safeguard. macOS-inspired timings are ClickShade's own preset tuned
// to the feel of Apple's locator (a couple of quick wiggles, fast growth to
// about 2.5x, quick return); Apple does not publish its detector or timing
// constants. No upstream implementation is included.
class ShakeLocator {
public:
    void Configure(int style, int sensitivity = 100, int maximumScale = 400) noexcept {
        style_ = std::clamp(style, 0, 2);
        sensitivity_ = std::clamp(sensitivity, 50, 200) / 100.0;
        ceiling_ = std::clamp(maximumScale, 150, 400) / 100.0;
        Reset();
    }

    void Reset() noexcept {
        ClearTrace();
        from_ = target_ = 1.0;
        begin_ = 0.0;
        holdUntil_ = 0.0;
        transition_ = 0.0;
    }

    void ClearTrace() noexcept { first_ = count_ = 0; }

    bool Observe(double x, double y, double now, bool eligible = true) noexcept {
        if (!style_ || !eligible || !std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(now)) {
            ClearTrace();
            return false;
        }
        const double window = style_ == 1 ? 1.0 : 0.60;
        if (count_ && now < At(count_ - 1).time) ClearTrace();
        while (count_ && now - At(0).time >= window) {
            first_ = (first_ + 1) % trace_.size();
            --count_;
        }
        const Point sample{x, y, now};
        if (count_ >= 2) {
            const auto& a = At(count_ - 2);
            auto& b = At(count_ - 1);
            // Retain turns and endpoints, rather than allocating every input
            // report. A one-pixel tolerance avoids noise changing direction.
            const auto compatible = [](double previous, double next) {
                return !(previous > 1.0 && next < -1.0) &&
                       !(previous < -1.0 && next > 1.0);
            };
            if (compatible(b.x - a.x, x - b.x) && compatible(b.y - a.y, y - b.y)) {
                b = sample;
                return false;
            }
        }
        if (count_ == trace_.size()) {
            first_ = (first_ + 1) % trace_.size();
            --count_;
        }
        At(count_++) = sample;
        if (count_ < 3) return false;
        double lowX = x, highX = x, lowY = y, highY = y, path = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& p = At(i);
            lowX = std::min(lowX, p.x); highX = std::max(highX, p.x);
            lowY = std::min(lowY, p.y); highY = std::max(highY, p.y);
            if (i) path += std::hypot(p.x - At(i - 1).x, p.y - At(i - 1).y);
        }
        const double span = std::hypot(highX - lowX, highY - lowY);
        // macOS-inspired: two quick back-and-forth passes over about 60 px
        // within 600 ms are enough, which is how little Apple's locator needs.
        const double minimumSpan = (style_ == 1 ? 100.0 : 60.0) / std::sqrt(sensitivity_);
        const double requiredRatio = (style_ == 1 ? 4.0 : 3.0) / sensitivity_;
        if (span < minimumSpan || path <= span * requiredRatio) return false;

        const double current = Scale(now);
        const double initial = style_ == 1 ? 3.0 : 2.5;
        const double next = target_ > 1.0 ? target_ + 1.0 : initial;
        const double desired = std::min(ceiling_, next);
        if (desired != target_) {
            from_ = current;
            target_ = desired;
            begin_ = now;
            transition_ = style_ == 1 ? 0.200 : 0.120;
        }
        holdUntil_ = now + (style_ == 1 ? 2.0 : 0.8);
        ClearTrace();
        return true;
    }

    double Scale(double now) noexcept {
        if (!style_ || !std::isfinite(now)) return 1.0;
        if (holdUntil_ > 0.0 && now >= holdUntil_) {
            from_ = Interpolate(holdUntil_);
            target_ = 1.0;
            begin_ = holdUntil_;
            transition_ = style_ == 1 ? 0.200 : 0.220;
            holdUntil_ = 0.0;
        }
        return Interpolate(now);
    }

    bool Active(double now) noexcept {
        return Scale(now) > 1.0 || target_ > 1.0;
    }

    // Absolute seconds until the next useful update. The hold phase needs one
    // deadline, and a completely idle locator needs no timer at all.
    double NextWake(double now, double frameInterval) noexcept {
        (void)Scale(now);
        if (now < begin_ + transition_ && from_ != target_)
            return std::min(begin_ + transition_, now + std::clamp(frameInterval, 1.0 / 360.0, 1.0 / 30.0));
        if (holdUntil_ > now) return holdUntil_;
        return std::numeric_limits<double>::infinity();
    }

    std::size_t TraceSize() const noexcept { return count_; }

private:
    struct Point { double x = 0.0, y = 0.0, time = 0.0; };
    std::array<Point, 256> trace_{};
    std::size_t first_ = 0, count_ = 0;
    int style_ = 0;
    double sensitivity_ = 1.0, ceiling_ = 4.0;
    double from_ = 1.0, target_ = 1.0, begin_ = 0.0;
    double transition_ = 0.0, holdUntil_ = 0.0;

    Point& At(std::size_t index) noexcept { return trace_[(first_ + index) % trace_.size()]; }

    double Interpolate(double now) const noexcept {
        if (transition_ <= 0.0 || now >= begin_ + transition_) return target_;
        const double progress = std::clamp((now - begin_) / transition_, 0.0, 1.0);
        const double eased = progress < 0.5 ? 4.0 * progress * progress * progress
            : 1.0 - std::pow(2.0 - 2.0 * progress, 3.0) / 2.0;
        return from_ + (target_ - from_) * eased;
    }
};

} // namespace clickshade
