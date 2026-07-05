#include "core/cadence.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace lsfg {

// A gap longer than this is a pause or seek, not cadence; the measurement
// window restarts. Anything slower than 2 fps is not frame-gen material.
static constexpr double GAP_RESET_S = 0.5;
static constexpr size_t UNIQUE_WINDOW = 48;  // ~2 s of 24 fps video
static constexpr size_t FRAME_WINDOW = 240;  // ~4 s of a 60 Hz stream
static constexpr size_t RUN_WINDOW = 32;
static constexpr size_t MIN_INTERVALS = 8;   // uniques before fps is reported
static constexpr size_t MIN_RUNS = 8;        // runs before a pattern is named

void CadenceTracker::addFrame(double t_seconds, bool duplicate) {
    total_frames_++;
    if (have_last_ && t_seconds - last_t_ > GAP_RESET_S) {
        frames_.clear();
        uniques_.clear();
        runs_.clear();
        current_run_ = 0;
    }
    last_t_ = t_seconds;
    have_last_ = true;

    frames_.push_back({t_seconds, duplicate});
    if (frames_.size() > FRAME_WINDOW)
        frames_.pop_front();

    if (duplicate) {
        if (current_run_ > 0) // a duplicate before any unique frame is noise
            current_run_++;
        return;
    }

    unique_frames_++;
    if (current_run_ > 0) {
        runs_.push_back(current_run_);
        if (runs_.size() > RUN_WINDOW)
            runs_.pop_front();
    }
    current_run_ = 1;
    uniques_.push_back(t_seconds);
    if (uniques_.size() > UNIQUE_WINDOW)
        uniques_.pop_front();
}

// Rate over a timestamp window. Pulldown alternates short and long unique
// intervals (33/50 ms for 3:2), so the span mean is the right estimator;
// a median would return one of the two wrong values.
static double windowFps(double t_first, double t_last, size_t n) {
    if (n < 2 || t_last <= t_first)
        return 0.0;
    return double(n - 1) / (t_last - t_first);
}

// Most common value and its count.
static std::pair<int, int> mode(const std::vector<int>& v) {
    std::map<int, int> counts;
    for (int x : v)
        counts[x]++;
    int best = v.empty() ? 0 : v[0], n = 0;
    for (const auto& [val, cnt] : counts) {
        if (cnt > n) {
            n = cnt;
            best = val;
        }
    }
    return {best, n};
}

// Classifies the repeat run-lengths: constant k -> "k:k", alternating a/b
// -> "a:b" (3:2 pulldown), otherwise "irregular". Tolerates ~10% stray runs
// (scene overlays, probe noise).
static std::string patternLabel(const std::deque<int>& runs) {
    if (runs.size() < MIN_RUNS)
        return "?";
    std::vector<int> r(runs.begin(), runs.end());

    auto [m, n] = mode(r);
    if (n * 10 >= int(r.size()) * 9)
        return std::to_string(m) + ":" + std::to_string(m);

    std::vector<int> even, odd;
    for (size_t i = 0; i < r.size(); i++)
        (i % 2 ? odd : even).push_back(r[i]);
    auto [m0, n0] = mode(even);
    auto [m1, n1] = mode(odd);
    if (m0 != m1 && n0 * 10 >= int(even.size()) * 9 &&
        n1 * 10 >= int(odd.size()) * 9)
        return std::to_string(std::max(m0, m1)) + ":" +
               std::to_string(std::min(m0, m1));
    return "irregular";
}

CadenceStats CadenceTracker::stats() const {
    CadenceStats s;
    s.total_frames = total_frames_;
    s.unique_frames = unique_frames_;

    if (frames_.size() >= 2) {
        s.capture_fps =
            windowFps(frames_.front().t, frames_.back().t, frames_.size());
        size_t dups = 0;
        for (const auto& f : frames_)
            if (f.duplicate)
                dups++;
        s.dup_ratio = double(dups) / double(frames_.size());
    }

    if (uniques_.size() >= MIN_INTERVALS + 1) {
        // Use a whole number of pulldown periods: with alternating short and
        // long intervals (3:2), an odd interval count skews the span mean.
        size_t n = uniques_.size();
        if ((n - 1) % 2 != 0)
            n--;
        s.source_fps =
            windowFps(uniques_[uniques_.size() - n], uniques_.back(), n);
    }

    // Damage-driven delivery (no duplicates ever) yields runs of all 1s and
    // classifies as "1:1" like a matched-rate stream — both are correct.
    s.pattern = patternLabel(runs_);

    // Locked when the first and second half of the unique window agree on
    // the rate; disagreement means the cadence changed and the window is
    // still flushing.
    if (s.source_fps > 0.0 && s.pattern != "irregular" && s.pattern != "?") {
        size_t half = uniques_.size() / 2;
        double f1 = windowFps(uniques_.front(), uniques_[half], half + 1);
        double f2 = windowFps(uniques_[half], uniques_.back(),
                              uniques_.size() - half);
        if (f1 > 0.0 && f2 > 0.0 &&
            std::abs(f1 - f2) / s.source_fps <= 0.03)
            s.locked = true;
    }
    return s;
}

void CadenceTracker::reset() {
    frames_.clear();
    uniques_.clear();
    runs_.clear();
    current_run_ = 0;
    have_last_ = false;
    last_t_ = 0.0;
    total_frames_ = 0;
    unique_frames_ = 0;
}

} // namespace lsfg
