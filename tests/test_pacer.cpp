// Unit tests for FramePacer. Plain CTest executable: returns nonzero on
// any failure. No framework — see PLAN.md.

#include "core/pacer.hpp"

#include <cmath>
#include <cstdio>

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

using lsfg::FramePacer;
using lsfg::PaceDecision;
using Mode = PaceDecision::Mode;

// Feed unique arrivals at source_fps while sampling decide() at display_hz,
// and count how many times the on-screen state changes — the effective
// output frame rate. Arrivals are delivered before the display tick that
// follows them, like the capture thread beating the render thread.
static double measureOutputFps(double source_fps, double display_hz, int m,
                               double seconds) {
    FramePacer p;
    p.setMultiplier(m);
    p.setCadence(source_fps, true);

    uint64_t seq = 0;
    long arrivals = long(seconds * source_fps);
    long ticks = long(seconds * display_hz);
    long next_arrival = 0;
    long changes = 0;
    bool have_prev = false;
    PaceDecision prev;

    for (long i = 0; i < ticks; i++) {
        // quarter-tick offset: real display clocks are not phase-locked to
        // the source, and sampling exactly on step boundaries is degenerate
        double t = (double(i) + 0.25) / display_hz;
        while (next_arrival < arrivals &&
               double(next_arrival) / source_fps <= t) {
            p.onUniqueFrame(++seq, double(next_arrival) / source_fps);
            next_arrival++;
        }
        PaceDecision d = p.decide(t);
        bool same = have_prev && d.mode == prev.mode && d.step == prev.step &&
                    d.seq_a == prev.seq_a && d.seq_b == prev.seq_b;
        if (!same)
            changes++;
        prev = d;
        have_prev = true;
    }
    return double(changes) / seconds;
}

static void testHoldThenPassthrough() {
    FramePacer p;
    p.setMultiplier(2);
    CHECK(p.decide(0.0).mode == Mode::Hold);
    p.onUniqueFrame(1, 0.0);
    // one unique frame is not a pair, locked or not
    p.setCadence(24.0, true);
    CHECK(p.decide(0.01).mode == Mode::Passthrough);
}

static void testPassthroughWhenUnlocked() {
    FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, false);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, 1.0 / 24.0);
    CHECK(p.decide(1.0 / 24.0 + 0.01).mode == Mode::Passthrough);
    p.setCadence(24.0, true);
    CHECK(p.decide(1.0 / 24.0 + 0.01).mode == Mode::Interpolate);
}

static void testPassthroughAtMultiplierOne() {
    FramePacer p;
    p.setMultiplier(1);
    p.setCadence(24.0, true);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, 1.0 / 24.0);
    CHECK(p.decide(1.0 / 24.0 + 0.01).mode == Mode::Passthrough);
}

static void testQuantizedPhases() {
    const double period = 1.0 / 24.0;
    FramePacer p;
    p.setMultiplier(4);
    p.setCadence(24.0, true);
    p.onUniqueFrame(7, 0.0);
    p.onUniqueFrame(8, period);

    PaceDecision d = p.decide(period); // pair window starts at B's arrival
    CHECK(d.mode == Mode::Interpolate);
    CHECK(d.step == 0);
    CHECK_NEAR(d.phase, 0.0, 1e-9); // step 0 is exactly frame A
    CHECK(d.seq_a == 7 && d.seq_b == 8);

    d = p.decide(period + 0.30 * period);
    CHECK(d.step == 1);
    CHECK_NEAR(d.phase, 0.25, 1e-9);

    d = p.decide(period + 0.90 * period);
    CHECK(d.step == 3);
    CHECK_NEAR(d.phase, 0.75, 1e-9);

    // window exhausted: the latest real frame is correct again
    CHECK(p.decide(period + 1.01 * period).mode == Mode::Passthrough);
}

static void testPhaseMonotonicWithinPair() {
    const double period = 1.0 / 30.0;
    FramePacer p;
    p.setMultiplier(4);
    p.setCadence(30.0, true);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, period);
    double last_phase = -1.0;
    for (int i = 0; i < 40; i++) {
        PaceDecision d = p.decide(period + double(i) / 40.0 * period);
        CHECK(d.mode == Mode::Interpolate);
        CHECK(d.phase >= last_phase);
        CHECK(d.phase >= 0.0 && d.phase < 1.0);
        last_phase = d.phase;
    }
}

static void testPauseHoldsLatestFrame() {
    const double period = 1.0 / 24.0;
    FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, true);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, period);
    CHECK(p.decide(period + 0.6 * period).mode == Mode::Interpolate);
    // arrivals stop (pause): hold the real frame, do not blend stale history
    CHECK(p.decide(period + 0.7).mode == Mode::Passthrough);
    // first frame after the pause must not pair across the gap
    p.onUniqueFrame(3, period + 2.0);
    CHECK(p.decide(period + 2.0 + 0.2 * period).mode == Mode::Passthrough);
    p.onUniqueFrame(4, period + 2.0 + period);
    PaceDecision d = p.decide(period + 2.0 + period + 0.6 * period);
    CHECK(d.mode == Mode::Interpolate);
    CHECK(d.seq_a == 3 && d.seq_b == 4);
}

static void testOutputRate2x24() {
    // 24 fps source, 2x -> 48 distinct outputs/s on a 60 Hz display loop
    CHECK_NEAR(measureOutputFps(24.0, 60.0, 2, 4.0), 48.0, 2.5);
}

static void testOutputRate3x20() {
    // 20 fps source, 3x -> 60/s; every display tick shows a new state
    CHECK_NEAR(measureOutputFps(20.0, 60.0, 3, 4.0), 60.0, 2.5);
}

static void testOutputRate4x24At240() {
    // sampling fast enough to resolve all steps: 24 x 4 = 96/s
    CHECK_NEAR(measureOutputFps(24.0, 240.0, 4, 4.0), 96.0, 3.0);
}

static void testSourceRateDrift() {
    // cadence re-estimate mid-stream changes the period without a glitch
    FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, true);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, 1.0 / 24.0);
    CHECK(p.decide(1.0 / 24.0 + 0.02).mode == Mode::Interpolate);
    p.setCadence(30.0, true);
    PaceDecision d = p.decide(1.0 / 24.0 + 0.02);
    CHECK(d.mode == Mode::Interpolate); // 0.02 < 1/30
    CHECK(d.step == 1);                 // 0.02 / (1/30) * 2 = 1.2
}

static void testMultiplierClamp() {
    FramePacer p;
    p.setMultiplier(0);
    CHECK(p.multiplier() == 1);
    p.setMultiplier(9);
    CHECK(p.multiplier() == 4);
}

static void testReset() {
    FramePacer p;
    p.setMultiplier(2);
    p.setCadence(24.0, true);
    p.onUniqueFrame(1, 0.0);
    p.onUniqueFrame(2, 1.0 / 24.0);
    p.reset();
    CHECK(p.decide(1.0 / 24.0 + 0.01).mode == Mode::Hold);
}

int main() {
    testHoldThenPassthrough();
    testPassthroughWhenUnlocked();
    testPassthroughAtMultiplierOne();
    testQuantizedPhases();
    testPhaseMonotonicWithinPair();
    testPauseHoldsLatestFrame();
    testOutputRate2x24();
    testOutputRate3x20();
    testOutputRate4x24At240();
    testSourceRateDrift();
    testMultiplierClamp();
    testReset();
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
