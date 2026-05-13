// Monomorphic: tight malloc(48) / free in a loop. Same allocation site,
// same size, churn pattern. Hits BumpAlloc's deopt path (region exhaust)
// repeatedly unless EpochArena or TLFreeList is forced.
//
// Run standalone for glibc baseline; under LD_PRELOAD with libtbjit
// (optionally with TBJIT_FORCE_STRATEGY=arena or freelist) for the JIT
// path.

#include "bench_common.h"
#include <cstdlib>
#include <cstring>

int main() {
    constexpr int    WARMUP_ITERS  = 30'000;
    constexpr int    MEASURE_ITERS = 1'000'000;
    constexpr size_t SIZE          = 48;

    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        unsigned char* p = static_cast<unsigned char*>(malloc(SIZE));
        if (!p) return 1;
        memset(p, g_pattern, SIZE);
        escape(p);
        g_sink += p[0] + p[SIZE - 1];
        free(p);
        pace(i);
    }
    clock_gettime(CLOCK_MONOTONIC, &w1);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < MEASURE_ITERS; ++i) {
        unsigned char* p = static_cast<unsigned char*>(malloc(SIZE));
        if (!p) return 1;
        memset(p, g_pattern, SIZE);
        escape(p);
        g_sink += p[0] + p[SIZE - 1];
        free(p);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    report("warmup:",  elapsed_ns(w0, w1), WARMUP_ITERS,  ", 48B, paced");
    report("steady:",  elapsed_ns(t0, t1), MEASURE_ITERS, ", 48B");
    return 0;
}
