// Performance protection. MoonUp must never cost the game more than it gives back, and it must
// never show fewer frames than the game renders. Frame rate comes first, quality second.
//
// Two signals are used:
//  * the outcome: the game's frame rate while our overlay covers it, compared with what it reached
//    uncovered (measured at the start and whenever the overlay is hidden). Only frames the display
//    can show count: a game going from 400 to 200 fps on a 144 Hz screen loses nothing visible.
//  * MoonUp' own GPU time (timestamp queries, estimated per processed / generated / presented frame).
// When either says "too expensive", the effects are lowered step by step: lighter motion search ->
// lighter upscaler (neural -> FSR) -> Neural Render / Vision off -> frame generation off ->
// processing rate limit -> bypass (the game is shown directly). A collapse (the game falls far
// below its own rate) is handled at once, one step per measurement window.
// After a while of comfortable operation it tries to go back up one step. If the game collapses
// again right after coming back, MoonUp stays bypassed for the rest of the session (sticky).
#pragma once
#include <algorithm>
#include <cmath>

namespace sw {

class PerfGovernor {
public:
    static constexpr double kBudget = 0.30;  // share of the GPU time MoonUp may use
    // Levels.
    static constexpr int kLightMotion = 1, kLightUpscale = 2, kNoEffects = 3, kNoFrameGen = 4, kRateLimit = 5,
                         kMaxTier = 6;

    void Reset() { *this = PerfGovernor(); }

    // Called every ~0.5 s with the measurements of the last window.
    //  gameFps   frames the game delivered per second
    //  procFps   frames MoonUp processed per second
    //  busy      estimated GPU time used by MoonUp in the window / window length
    //  fgWanted  frame generation is configured
    //  heavyFx   a lighter upscaler or disabling effects can still save time (levels 2, 3 useful)
    //  covering  our overlay has been covering the game for a while (outcome is meaningful)
    //  displayHz the rate the display can show
    // Returns true when the level changed.
    //  fgMult    frame generation multiplier in effect (1 = off). With frame generation the game's own
    //            rate may drop somewhat: what counts is that the shown rate beats the game alone.
    bool Update(double window, double gameFps, double procFps, double busy, bool fgWanted, bool heavyFx, bool covering,
                double displayHz, double fgMult = 1.0) {
        gameFps_ = gameFps_ <= 0 ? gameFps : gameFps_ * 0.5 + gameFps * 0.5;
        procFps_ = procFps;
        age_ += window;
        sinceReturn_ += window;
        fgWanted_ = fgWanted;
        heavyFx_ = heavyFx;
        collapse_ = false;

        // Uncovered windows tell what the game reaches alone.
        if (!covering && gameFps > 5) {
            uncoveredT_ += window;
            if (uncoveredT_ >= 0.5) native_ = native_ <= 0 ? gameFps : native_ * 0.7 + gameFps * 0.3;
        } else {
            uncoveredT_ = 0;
        }

        if (tier_ >= kMaxTier) {
            // Bypassed: nothing is processed, so only time (or a slower game) brings us back.
            if (sticky_) return false;
            if (age_ >= hold_ || (bypassGame_ > 0 && gameFps_ < bypassGame_ * 0.6)) return Step(-1);
            return false;
        }

        // Rate limit (only on its own level): keep the busy share under the budget.
        if (tier_ == kRateLimit) {
            if (busy > kBudget * 1.05) {
                double base = std::max(procFps, 8.0);
                cap_ = std::max(20.0, base * kBudget / busy * 0.95);
            } else if (cap_ > 0 && busy < kBudget * 0.6 && !hurt_) {
                cap_ = cap_ * 1.15 + 2.0;
                if (cap_ > std::max(gameFps_ * 1.5, 60.0)) cap_ = 0;
            }
        } else {
            cap_ = 0;
        }

        // Outcome: what the display could show from the game alone vs. what it gets now.
        double ref = native_ > 0 ? std::min(native_, displayHz > 20 ? displayHz : native_) : 0;
        const bool fg = fgMult > 1.01;
        if (fg) {
            double shown = std::min(gameFps_ * fgMult, displayHz > 20 ? displayHz : 1e9);
            hurt_ = covering && ref > 20 && (shown < ref * 1.1 || gameFps_ < native_ * 0.6);
            collapse_ = covering && ref > 20 && gameFps_ < native_ * 0.4;
        } else {
            hurt_ = covering && ref > 20 && gameFps_ < ref * 0.75;
            collapse_ = covering && ref > 20 && gameFps_ < ref * 0.5;
        }
        const double budget = fg ? kBudget * 1.4 : kBudget;
        // Severe collapse (the game at a fraction of its rate, e.g. 400 -> 10 fps): lighter effects
        // will not repair that. Stop everything costly at once, and step aside if that is not enough.
        if (covering && ref > 20 && gameFps < ref * 0.3 && age_ >= 0.5) {
            int to = tier_ < kNoFrameGen ? kNoFrameGen : kMaxTier;
            if (to == kNoFrameGen && !fgWanted_ && !heavyFx_) to = kMaxTier;
            bool r = Jump(to);
            if (r && tier_ == kMaxTier && !sticky_) hold_ = 20.0;  // one more try later, then sticky
            return r;
        }

        // The first second after a start or a level change is only observed (shader warm-up,
        // first-use allocations, the game settling).
        if (age_ < 1.0) return false;
        bool overBudget = busy > budget;

        if (collapse_) {
            // Severe: one step per window. On the rate-limit level, halve the limit first.
            bad_ = 0;
            if (tier_ == kRateLimit && (cap_ <= 0 || cap_ > 25)) {
                cap_ = cap_ <= 0 ? std::min(std::max(procFps * 0.5, 20.0), 60.0) : std::max(20.0, cap_ * 0.5);
                return false;
            }
            return Step(+1);
        }
        if (hurt_ || overBudget) {
            if (++bad_ >= 2) {
                bad_ = 0;
                return Step(+1);
            }
        } else {
            bad_ = 0;
        }

        // Go back up after a while of comfortable operation.
        if (tier_ > 0) {
            if (!hurt_ && busy < kBudget * 0.55 && (cap_ <= 0 || cap_ >= gameFps_)) comfyT_ += window;
            else comfyT_ = 0;
            if (comfyT_ >= hold_) {
                comfyT_ = 0;
                return Step(-1);
            }
        }
        return false;
    }

    // Frame generation makes no sense when the game alone already reaches the display rate.
    static bool FrameGenAllowed(double gameFps, double target) { return target <= 0 || gameFps < target * 0.9; }

    int tier() const { return tier_; }
    bool bypass() const { return tier_ >= kMaxTier; }
    bool sticky() const { return sticky_; }
    bool collapsed() const { return collapse_; }
    // Minimum time between two processed frames (0 = every frame).
    double interval() const { return cap_ > 0 ? 1.0 / cap_ : 0.0; }
    double cap() const { return cap_; }
    double gameFps() const { return gameFps_; }
    double native() const { return native_; }
    void SetNative(double fps) {
        if (fps > 5) native_ = fps;
    }

private:
    bool Useful(int n) const {
        if (n == kLightMotion || n == kNoFrameGen) return fgWanted_;
        if (n == kLightUpscale || n == kNoEffects) return heavyFx_;
        return true;
    }

    bool Jump(int to) {
        bool changed = false;
        while (tier_ < to && Step(+1)) changed = true;
        return changed;
    }

    bool Step(int d) {
        int n = std::clamp(tier_ + d, 0, kMaxTier);
        // Skip levels that change nothing for this configuration.
        while (n > 0 && n < kMaxTier && !Useful(n)) n += d > 0 ? 1 : -1;
        n = std::clamp(n, 0, kMaxTier);
        if (n == tier_) return false;
        if (d > 0) {
            // degraded: remember that going back up failed recently -> wait longer next time
            if (age_ < 40.0) failures_ = std::min(failures_ + 1, 3);
            if (n == kMaxTier) {
                bypassGame_ = gameFps_;
                // Came back from bypass only a few seconds ago and the game broke down again:
                // this game does not tolerate being covered. Stay out of its way.
                if (returned_ && sinceReturn_ < 8.0) sticky_ = true;
            }
        } else if (tier_ == kMaxTier) {
            returned_ = true;
            sinceReturn_ = 0;
        }
        tier_ = n;
        age_ = 0;
        hold_ = 8.0 * (1 << failures_);
        cap_ = tier_ == kRateLimit ? std::clamp(procFps_ * 0.8, 30.0, 60.0) : 0.0;
        bad_ = 0;
        comfyT_ = 0;
        return true;
    }

    bool fgWanted_ = true, heavyFx_ = true;
    int tier_ = 0;
    double cap_ = 0;  // processing rate limit (level kRateLimit only), 0 = none
    double native_ = 0, uncoveredT_ = 0;
    double gameFps_ = 0, procFps_ = 0;
    double age_ = 0, hold_ = 8.0, comfyT_ = 0, bypassGame_ = 0, sinceReturn_ = 1e9;
    int bad_ = 0, failures_ = 0;
    bool hurt_ = false, collapse_ = false, returned_ = false, sticky_ = false;
};

}  // namespace sw
