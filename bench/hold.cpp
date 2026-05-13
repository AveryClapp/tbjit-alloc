// Hold pattern: hot call site allocates 48-byte chunks and *never frees
// them* during the measurement loop (a single big free at the end).
// free_count/event_count stays well below LIFETIME_HOLD_RATIO (0.10), so
// the analyzer tags the site LifetimeTag::Hold. The reaper consequently
// will not reclaim segments — verifying that long-lived pools don't suffer
// the throughput overhead of speculative reclaim.

#include "bench_common.h"
#include <cstdlib>
#include <cstring>
#include <vector>

int main() {
    constexpr int    WARMUP_ITERS  = 30'000;
    constexpr int    MEASURE_ITERS = 500'000;
    constexpr size_t SIZE          = 48;

    std::vector<unsigned char*> kept;
    kept.reserve(WARMUP_ITERS + MEASURE_ITERS);

    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        auto* p = static_cast<unsigned char*>(malloc(SIZE));
        if (!p) return 1;
        memset(p, g_pattern, SIZE);
        escape(p);
        g_sink += p[0];
        kept.push_back(p);
        pace(i);
    }
    clock_gettime(CLOCK_MONOTONIC, &w1);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < MEASURE_ITERS; ++i) {
        auto* p = static_cast<unsigned char*>(malloc(SIZE));
        if (!p) return 1;
        memset(p, g_pattern, SIZE);
        escape(p);
        g_sink += p[0];
        kept.push_back(p);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    report("warmup:", elapsed_ns(w0, w1), WARMUP_ITERS,  ", 48B, no-free, paced");
    report("steady:", elapsed_ns(t0, t1), MEASURE_ITERS, ", 48B, no-free");

    // Cleanup outside the measured window so the deallocation cost doesn't
    // contaminate the steady-state number.
    for (auto* p : kept) free(p);
    return 0;
}
