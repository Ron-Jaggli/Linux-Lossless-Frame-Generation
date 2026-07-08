// Unit tests for FramePacer. Plain CTest executable: returns nonzero on any
// failure. Same harness style as test_cadence.cpp.

#include "core/cadence.hpp"
#include "core/pacer.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double va = (a), vb = (b);                                             \
        if (std::abs(va - vb) > (tol)) {                                       \
            std::printf("FAIL %s:%d: %s = %f, expected %f +/- %f\n", __FILE__, \
                        __LINE__, #a, va, vb, double(tol));                    \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// Feed unique frames at source_fps for `seconds`, sampling decide() at
// display_hz between arrivals. Returns the count of distinct presented
// outputs (a passthrough hold and each distinct (pair, phase) blend count
// once) and verifies phase is monotonic and in [0, 1) within each pair.
struct SimStats {
    int distinct_outputs = 0;
    int passthrough_ticks = 0;
    int interpolated_ticks = 0;
};

static SimStats simulate(lsfg::FramePacer& pacer, double source_fps,
                         double display_hz, double seconds, uint64_t& seq,
                         double t0 = 0.0, bool feed_frames = true) {
    SimStats st;
    std::set<std::pair<uint64_t, int>> outputs; // (seq_b, phase in 1/1000ths)
    double last_phase = -1.0;
    uint64_t last_pair = 0;
    long n_arrivals = feed_frames ? long(seconds * source_fps) : 0;
    long n_ticks = long(seconds * display_hz);
    long next_arrival = 0;
    for (long i = 0; i < n_ticks; i++) {
        double t = t0 + double(i) / display_hz;
        while (next_arrival < n_arrivals &&
               t0 + double(next_arrival) / source_fps <= t) {
            pacer.onUniqueFrame(++seq, t0 + double(next_arrival) / source_fps);
            next_arrival++;
        }
        auto d = pacer.decide(t);
        if (d.interpolate) {
            st.interpolated_ticks++;
            CHECK(d.phase >= 0.0 && d.phase < 1.0);
            CHECK(d.seq_b == d.seq_a + 1);
            if (d.seq_b == last_pair)
                CHECK(d.phase >= last_phase); // monotonic within a pair
            last_pair = d.seq_b;
            last_phase = d.phase;
            outputs.insert({d.seq_b, int(d.phase * 1000.0 + 0.5)});
        } else {
            st.passthrough_ticks++;
        }
    }
    st.distinct_outputs = int(outputs.size());
    return st;
}

static void test2xAt24() {
    lsfg::FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, true);
    uint64_t seq = 0;
    auto st = simulate(p, 24.0, 240.0, 4.0, seq);
    // ~48 distinct outputs/s for 4 s; the first pair needs two arrivals
    CHECK_NEAR(st.distinct_outputs, 4 * 48, 4);
    CHECK(st.interpolated_ticks > 0);
}

static void test3xAnd4x() {
    for (int m : {3, 4}) {
        lsfg::FramePacer p;
        p.setMultiplier(m);
        p.setCadence(24.0, true);
        uint64_t seq = 0;
        auto st = simulate(p, 24.0, 480.0, 4.0, seq);
        CHECK_NEAR(st.distinct_outputs, 4 * 24 * m, m * 2);
    }
}

static void test2xAt30() {
    lsfg::FramePacer p;
    p.setMultiplier(2);
    p.setCadence(30.0, true);
    uint64_t seq = 0;
    auto st = simulate(p, 30.0, 240.0, 4.0, seq);
    CHECK_NEAR(st.distinct_outputs, 4 * 60, 4);
}

static void testPassthroughWhenMultiplierOne() {
    lsfg::FramePacer p;
    p.setMultiplier(1);
    p.setCadence(24.0, true);
    uint64_t seq = 0;
    auto st = simulate(p, 24.0, 240.0, 2.0, seq);
    CHECK(st.interpolated_ticks == 0);
}

static void testPassthroughWhenUnlocked() {
    lsfg::FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, false); // estimate present but not stable
    uint64_t seq = 0;
    auto st = simulate(p, 24.0, 240.0, 2.0, seq);
    CHECK(st.interpolated_ticks == 0);
}

static void testPassthroughBeforePair() {
    lsfg::FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, true);
    p.onUniqueFrame(1, 0.0); // only one unique frame so far
    auto d = p.decide(0.02);
    CHECK(!d.interpolate);
}

static void testPauseHoldsLastFrame() {
    lsfg::FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, true);
    uint64_t seq = 0;
    simulate(p, 24.0, 240.0, 2.0, seq);
    // Arrivals stop; once the last pair's window is played out every
    // decision is passthrough (hold the latest real frame).
    auto st = simulate(p, 24.0, 240.0, 2.0, seq, 2.0, false);
    CHECK(st.interpolated_ticks <= 240 / 24 + 1); // at most one period's worth
    CHECK(st.passthrough_ticks > 400);
}

static void testResumeAfterPause() {
    lsfg::FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, true);
    uint64_t seq = 0;
    simulate(p, 24.0, 240.0, 1.0, seq);
    simulate(p, 24.0, 240.0, 1.0, seq, 3.0, false); // 2 s pause
    auto st = simulate(p, 24.0, 240.0, 2.0, seq, 4.0);
    CHECK_NEAR(st.distinct_outputs, 2 * 48, 5);
}

static void testDriftTracksSourceRate() {
    // The source estimate drifts (23.976 vs 24.0); the schedule follows it.
    lsfg::FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24000.0 / 1001.0, true);
    uint64_t seq = 0;
    auto st = simulate(p, 24000.0 / 1001.0, 240.0, 4.0, seq);
    CHECK_NEAR(st.distinct_outputs, int(4 * 2 * 24000.0 / 1001.0), 5);
}

static void testWithCadenceTrackerEndToEnd() {
    // Full loop: a 60 Hz pulldown stream feeds the tracker; the pacer takes
    // the tracker's estimate and unique arrivals, like the app wires it.
    lsfg::CadenceTracker ct;
    lsfg::FramePacer p;
    p.setMultiplier(2);
    uint64_t seq = 0;
    std::set<std::pair<uint64_t, int>> outputs;
    long prev_src = -1;
    int interpolated = 0;
    for (long i = 0; i < 60 * 6; i++) {
        double t = double(i) / 60.0;
        long src = long(std::floor(t * 24.0 + 1e-9));
        bool dup = src == prev_src;
        prev_src = src;
        ct.addFrame(t, dup);
        if (!dup)
            p.onUniqueFrame(++seq, t);
        auto cs = ct.stats();
        p.setCadence(cs.source_fps, cs.locked);
        // display refreshes between this capture tick and the next
        for (int k = 0; k < 4; k++) {
            auto d = p.decide(t + k / 240.0);
            if (d.interpolate) {
                interpolated++;
                outputs.insert({d.seq_b, int(d.phase * 1000.0 + 0.5)});
            }
        }
    }
    CHECK(interpolated > 0);
    // Once locked (~2 s in), ~48 distinct outputs/s; allow the lock-in ramp.
    CHECK(int(outputs.size()) > 3 * 48);
    CHECK(int(outputs.size()) <= 6 * 48 + 2);
}

static void testResetForgetsState() {
    lsfg::FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, true);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, 1.0 / 24.0);
    p.reset();
    p.setCadence(24.0, true);
    auto d = p.decide(2.0 / 24.0);
    CHECK(!d.interpolate); // no pair known after reset
}

int main() {
    test2xAt24();
    test3xAnd4x();
    test2xAt30();
    testPassthroughWhenMultiplierOne();
    testPassthroughWhenUnlocked();
    testPassthroughBeforePair();
    testPauseHoldsLastFrame();
    testResumeAfterPause();
    testDriftTracksSourceRate();
    testWithCadenceTrackerEndToEnd();
    testResetForgetsState();
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
