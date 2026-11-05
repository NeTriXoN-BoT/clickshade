#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace clickshade {

// Pointer lead for the effects that follow the pointer (halo, spotlight and
// magnifier). They reach the screen with the next composition, a few
// milliseconds after the pointer was read, and the pointer has moved on by
// then. During steady fast motion they are drawn about that far ahead.
//
// - Speed and direction come from a least-squares fit over the recent frames
//   that saw a new pointer position, so a mouse that reports less often than
//   frames are drawn, or unevenly, still gives a steady estimate.
// - The lead fades in over a few frames and fades out quickly for slow,
//   braking or turning motion, so precise aiming is never shifted.
// - A frame that sees no new position (an event without motion, a report
//   that has not arrived yet) keeps the lead. A stop is a pause longer than
//   the pointer's own report spacing: Settle() then drops the lead at once,
//   so the effects come to rest exactly on the pointer.
//
// Pure arithmetic on the renderer's samples (times in seconds, positions in
// physical pixels), so tests can drive it with simulated mice and displays.
class PointerLead {
public:
    struct Offset { double x = 0.0, y = 0.0; };

    // frameSeconds: the motion frame interval; scale: monitor DPI / 96.
    void Configure(double frameSeconds, double scale) noexcept {
        frame_ = std::clamp(frameSeconds, 1.0 / 1000.0, 0.1);
        scale_ = std::max(0.25, scale);
        Reset();
    }

    void Reset() noexcept {
        count_ = next_ = 0;
        amount_ = 0.0;
        offset_ = {};
    }

    bool Active() const noexcept {
        return std::isfinite(offset_.x) && std::isfinite(offset_.y) && (offset_.x != 0.0 || offset_.y != 0.0);
    }
    Offset Current() const noexcept { return offset_; }
    double LeadSeconds() const noexcept { return std::min(kLeadFraction * frame_, kMaximumLead); }

    // While Active(): when Settle() drops the lead if no new position comes.
    double SettleTime() const noexcept { return count_ ? Newest().t + StopGrace() : 0.0; }

    // A motion frame at time t with the pointer at (x, y).
    Offset Update(double t, double x, double y) noexcept {
        if (count_) {
            const Sample& last = Newest();
            if (t - last.t > kForgetSeconds) Reset();
            else if ((x == last.x && y == last.y) || t <= last.t) return offset_;
        }
        samples_[next_] = {t, x, y};
        next_ = (next_ + 1) % samples_.size();
        count_ = std::min(count_ + 1, samples_.size());

        Velocity full, recent;
        const bool fitted = Fit(full, recent);
        const double target = fitted ? TargetAmount(full, recent) : 0.0;
        const double previous = amount_;
        // Fade in over about three frames and out over a few: one unevenly
        // reported frame barely shows, real braking still ends the lead.
        if (target >= amount_) amount_ = std::min(target, amount_ + kRisePerFrame);
        else amount_ = std::max(target, amount_ * kFallPerFrame);
        if (amount_ < kNegligible) amount_ = 0.0;
        if (amount_ <= 0.0) { offset_ = {}; return offset_; }
        // Too few recent samples (a stalled frame, a pause): no direction to
        // follow, so the last lead fades along its own direction.
        if (!fitted || !(std::hypot(full.x, full.y) > 0.0)) {
            const double keep = previous > 0.0 ? amount_ / previous : 0.0;
            offset_ = {offset_.x * keep, offset_.y * keep};
            if (!Active()) { amount_ = 0.0; offset_ = {}; }
            return offset_;
        }

        // Direction from the steadier full fit. Its speed too, unless the
        // recent motion is clearly slower (braking): then the recent speed,
        // so the older, faster motion never carries the effects past a stop.
        const double fullSpeed = std::hypot(full.x, full.y), recentSpeed = std::hypot(recent.x, recent.y);
        const double speed = recentSpeed < kBraking * fullSpeed ? recentSpeed : fullSpeed;
        const double length = std::min(speed * amount_ * LeadSeconds(), kMaximumOffset * scale_);
        offset_ = {full.x / fullSpeed * length, full.y / fullSpeed * length};
        return offset_;
    }

    // A frame without motion at time t. Returns whether a lead remains.
    bool Settle(double t) noexcept {
        if (!Active()) return false;
        if (count_ && t + 1e-6 < SettleTime()) return true;
        amount_ = 0.0;
        offset_ = {};
        return false;
    }

private:
    struct Sample { double t = 0.0, x = 0.0, y = 0.0; };
    struct Velocity { double x = 0.0, y = 0.0; };

    static constexpr double kLeadFraction = 0.5;    // Of a motion frame.
    static constexpr double kMaximumLead = 0.012;   // Seconds.
    static constexpr double kMaximumOffset = 40.0;  // Pixels at 96 DPI.
    static constexpr double kForgetSeconds = 0.1;   // A longer pause starts over.
    static constexpr double kRisePerFrame = 0.34;
    static constexpr double kFallPerFrame = 0.6;
    static constexpr double kNegligible = 0.05;
    static constexpr double kSlowSpeed = 700.0;     // Pixels per second at 96 DPI: no lead.
    static constexpr double kFullSpeed = 1600.0;    // Full lead from here.
    static constexpr double kBraking = 0.85;        // Recent/full speed below this is braking.

    const Sample& At(std::size_t back) const noexcept {
        return samples_[(next_ + 2 * samples_.size() - 1 - back) % samples_.size()];
    }
    const Sample& Newest() const noexcept { return At(0); }

    // The pointer's own report spacing as this renderer sees it: the longest
    // recent gap between new positions, never below a frame. Gaps far beyond
    // the typical one are pauses, not spacing.
    double Spacing() const noexcept {
        std::array<double, 6> gaps{};
        std::size_t used = 0;
        for (std::size_t back = 1; back < count_ && used < gaps.size(); ++back) gaps[used++] = At(back - 1).t - At(back).t;
        if (!used) return frame_;
        std::array<double, 6> sorted = gaps;
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(used));
        const double typical = sorted[used / 2];
        double spacing = frame_;
        for (std::size_t index = 0; index < used; ++index)
            if (gaps[index] <= 2.2 * std::max(typical, frame_)) spacing = std::max(spacing, gaps[index]);
        return std::min(spacing, 0.05);
    }

    double StopGrace() const noexcept { return std::clamp(1.25 * Spacing(), 1.25 * frame_, 0.06); }

    // Least-squares velocity over samples newer than `since`, stopping at a
    // gap that means the pointer paused. Needs `minimum` samples.
    bool Slope(double since, std::size_t minimum, double gapLimit, Velocity& velocity) const noexcept {
        std::size_t used = 0;
        double sumT = 0.0, sumX = 0.0, sumY = 0.0;
        for (std::size_t back = 0; back < count_; ++back) {
            const Sample& sample = At(back);
            if (sample.t < since) break;
            if (back > 0 && At(back - 1).t - sample.t > gapLimit) break;
            sumT += sample.t; sumX += sample.x; sumY += sample.y; ++used;
        }
        if (used < minimum) return false;
        const double meanT = sumT / used, meanX = sumX / used, meanY = sumY / used;
        double tt = 0.0, tx = 0.0, ty = 0.0;
        for (std::size_t back = 0; back < used; ++back) {
            const Sample& sample = At(back);
            const double dt = sample.t - meanT;
            tt += dt * dt; tx += dt * (sample.x - meanX); ty += dt * (sample.y - meanY);
        }
        if (tt <= 0.0) return false;
        velocity = {tx / tt, ty / tt};
        return true;
    }

    bool Fit(Velocity& full, Velocity& recent) const noexcept {
        const double spacing = Spacing();
        const double window = std::clamp(4.0 * spacing, 0.032, 0.064);
        const double gapLimit = std::max(2.5 * spacing, 3.0 * frame_);
        const double now = Newest().t;
        if (!Slope(now - window, 3, gapLimit, full)) return false;
        // The newer part of the window, for braking and turning. Fewer than
        // three samples there would mostly measure report aliasing.
        if (!Slope(now - 0.6 * window, 3, gapLimit, recent)) recent = full;
        return true;
    }

    double TargetAmount(const Velocity& full, const Velocity& recent) const noexcept {
        const double fullSpeed = std::hypot(full.x, full.y), recentSpeed = std::hypot(recent.x, recent.y);
        if (fullSpeed <= 0.0 || recentSpeed <= 0.0) return 0.0;
        // The slow-motion threshold uses the newer, more current speed.
        double amount = std::clamp((std::min(fullSpeed, recentSpeed) / scale_ - kSlowSpeed) / (kFullSpeed - kSlowSpeed), 0.0, 1.0);
        // Braking: the recent motion is slower than the whole window's.
        amount *= std::clamp((recentSpeed / fullSpeed - 0.50) / 0.30, 0.0, 1.0);
        // Turning: full lead up to about 20 degrees, none from about 37.
        const double turn = (full.x * recent.x + full.y * recent.y) / (fullSpeed * recentSpeed);
        amount *= std::clamp((turn - 0.80) / 0.14, 0.0, 1.0);
        return amount;
    }

    std::array<Sample, 16> samples_{};
    std::size_t count_ = 0, next_ = 0;
    double frame_ = 1.0 / 60.0, scale_ = 1.0;
    double amount_ = 0.0;
    Offset offset_{};
};

} // namespace clickshade
