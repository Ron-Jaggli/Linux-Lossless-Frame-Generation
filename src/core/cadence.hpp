#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace lsfg {

struct CadenceStats {
    double source_fps = 0.0;   // recovered unique-frame rate; 0 until enough data
    double capture_fps = 0.0;  // rate at which frames (incl. duplicates) arrive
    double dup_ratio = 0.0;    // fraction of duplicate frames in the window
    std::string pattern = "?"; // "3:2", "2:2", "1:1", "irregular", "?" (no lock yet)
    bool locked = false;       // estimate is stable enough to act on
    uint64_t total_frames = 0;
    uint64_t unique_frames = 0;
};

// Recovers the source frame cadence from a stream of (timestamp, duplicate)
// events. Handles both delivery styles: a fixed-rate compositor stream where
// repeated video frames arrive as duplicates (pulldown, e.g. 3:2 for 23.976
// fps in 60 Hz), and damage-driven delivery where duplicates never arrive
// and cadence comes from unique-frame arrival times alone. A gap longer
// than half a second (pause/seek) restarts the measurement window; lifetime
// counters keep counting. Not thread-safe; the caller serializes access.
class CadenceTracker {
public:
    void addFrame(double t_seconds, bool duplicate);
    CadenceStats stats() const;
    void reset();

private:
    struct Arrival {
        double t;
        bool duplicate;
    };

    std::deque<Arrival> frames_;  // recent arrivals, duplicates included
    std::deque<double> uniques_;  // timestamps of recent unique frames
    std::deque<int> runs_;        // completed repeat counts per unique frame
    int current_run_ = 0;         // displays of the current unique frame so far
    double last_t_ = 0.0;
    bool have_last_ = false;
    uint64_t total_frames_ = 0;
    uint64_t unique_frames_ = 0;
};

} // namespace lsfg
