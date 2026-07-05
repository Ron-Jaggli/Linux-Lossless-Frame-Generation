// Unit tests for CadenceTracker. Plain CTest executable: returns nonzero on
// any failure. No framework — see PLAN.md.

#include "core/cadence.hpp"

#include <cmath>
#include <cstdio>
#include <string>

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

#define CHECK_EQ_STR(a, b)                                                     \
    do {                                                                       \
        std::string va = (a), vb = (b);                                        \
        if (va != vb) {                                                        \
            std::printf("FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__,\
                        __LINE__, #a, va.c_str(), vb.c_str());                 \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// Deterministic jitter in [-amp, amp] (tiny LCG; std::random engines would
// also be deterministic but this keeps the tests self-contained).
struct Jitter {
    uint64_t state = 12345;
    double amp;
    explicit Jitter(double a) : amp(a) {}
    double next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return amp * (2.0 * double((state >> 33) & 0xFFFFFF) / 0xFFFFFF - 1.0);
    }
};

// Feed a compositor stream at capture_hz repainting a source_fps video:
// frame i at t0 + i/capture_hz is a duplicate unless the source frame index
// floor(t * source_fps) advanced. Returns the last timestamp fed.
static double feedPulldown(lsfg::CadenceTracker& ct, double source_fps,
                           double capture_hz, double seconds, double t0 = 0.0,
                           double jitter_amp = 0.0) {
    Jitter j(jitter_amp);
    long prev_src = -1;
    long n = long(seconds * capture_hz);
    double t = t0;
    for (long i = 0; i < n; i++) {
        double ideal = t0 + double(i) / capture_hz;
        long src = long(std::floor((ideal - t0) * source_fps + 1e-9));
        t = ideal + (jitter_amp > 0.0 ? j.next() : 0.0);
        ct.addFrame(t, src == prev_src);
        prev_src = src;
    }
    return t;
}

static void testExact24In60() {
    lsfg::CadenceTracker ct;
    feedPulldown(ct, 24.0, 60.0, 4.0);
    auto s = ct.stats();
    CHECK_NEAR(s.source_fps, 24.0, 0.1);
    CHECK_NEAR(s.capture_fps, 60.0, 1.0);
    CHECK_EQ_STR(s.pattern, "3:2");
    CHECK(s.locked);
    CHECK_NEAR(s.dup_ratio, 0.6, 0.03); // 24 unique of 60 shown
}

static void testNtsc23976In60() {
    lsfg::CadenceTracker ct;
    feedPulldown(ct, 24000.0 / 1001.0, 60.0, 6.0);
    auto s = ct.stats();
    CHECK_NEAR(s.source_fps, 23.976, 0.15);
    // the true NTSC cadence occasionally doubles a 3-run; either verdict on
    // the label is defensible, but the rate estimate must hold either way
    CHECK(s.pattern == "3:2" || s.pattern == "irregular");
}

static void test30In60WithJitter() {
    lsfg::CadenceTracker ct;
    feedPulldown(ct, 30.0, 60.0, 4.0, 0.0, 0.0015);
    auto s = ct.stats();
    CHECK_NEAR(s.source_fps, 30.0, 0.5);
    CHECK_EQ_STR(s.pattern, "2:2");
    CHECK(s.locked);
}

static void test60In60() {
    lsfg::CadenceTracker ct;
    feedPulldown(ct, 60.0, 60.0, 2.0);
    auto s = ct.stats();
    CHECK_NEAR(s.source_fps, 60.0, 0.5);
    CHECK_EQ_STR(s.pattern, "1:1");
    CHECK(s.locked);
    CHECK_NEAR(s.dup_ratio, 0.0, 0.01);
}

static void testDamageDrivenSparse() {
    // Compositor sends only changed frames: 24 fps arrivals, never duplicate.
    lsfg::CadenceTracker ct;
    for (int i = 0; i < 96; i++)
        ct.addFrame(double(i) / 24.0, false);
    auto s = ct.stats();
    CHECK_NEAR(s.source_fps, 24.0, 0.1);
    CHECK_EQ_STR(s.pattern, "1:1");
    CHECK(s.locked);
    CHECK_NEAR(s.dup_ratio, 0.0, 0.01);
    CHECK(s.unique_frames == 96);
    CHECK(s.total_frames == 96);
}

static void testCadenceChangeRelocks() {
    lsfg::CadenceTracker ct;
    double t = feedPulldown(ct, 24.0, 60.0, 4.0);
    feedPulldown(ct, 30.0, 60.0, 4.0, t + 1.0 / 60.0);
    auto s = ct.stats();
    CHECK_NEAR(s.source_fps, 30.0, 0.5);
    CHECK_EQ_STR(s.pattern, "2:2");
    CHECK(s.locked);
}

static void testPauseResume() {
    lsfg::CadenceTracker ct;
    double t = feedPulldown(ct, 24.0, 60.0, 2.0);
    // 2 s pause (stream stalls), then playback resumes
    feedPulldown(ct, 24.0, 60.0, 2.0, t + 2.0);
    auto s = ct.stats();
    CHECK_NEAR(s.source_fps, 24.0, 0.2); // the gap must not skew the estimate
    CHECK(s.locked);
    CHECK(s.total_frames == 240);
}

static void testColdStart() {
    lsfg::CadenceTracker ct;
    feedPulldown(ct, 24.0, 60.0, 0.05); // 3 frames
    auto s = ct.stats();
    CHECK_NEAR(s.source_fps, 0.0, 1e-9); // too early to report a rate
    CHECK(!s.locked);
    CHECK_EQ_STR(s.pattern, "?");
    CHECK(s.total_frames == 3);
}

static void testReset() {
    lsfg::CadenceTracker ct;
    feedPulldown(ct, 24.0, 60.0, 2.0);
    ct.reset();
    auto s = ct.stats();
    CHECK(s.total_frames == 0);
    CHECK(s.unique_frames == 0);
    CHECK_NEAR(s.source_fps, 0.0, 1e-9);
    CHECK(!s.locked);
}

int main() {
    testExact24In60();
    testNtsc23976In60();
    test30In60WithJitter();
    test60In60();
    testDamageDrivenSparse();
    testCadenceChangeRelocks();
    testPauseResume();
    testColdStart();
    testReset();
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
