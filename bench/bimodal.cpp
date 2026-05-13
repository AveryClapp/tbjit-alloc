// Bimodal pattern: same call site alternates between two sizes (32, 96).
// Drives the analyzer's top_k_modes; expected pick is MultiSizeFreeList.
// is_monomorphic(0.95) returns false (each size only covers ~50%), so
// BumpAlloc is off the table. ThreadLocalFreeList would deopt on the
// non-dominant size. Multi-class learns both and serves both fast.

#include "bench_common.h"
#include <cstdlib>
#include <cstring>

int main() {
    constexpr int WARMUP_ITERS  = 30'000;
    constexpr int MEASURE_ITERS = 1'000'000;
    constexpr size_t SIZES[] = {32, 96};

    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        size_t sz = SIZES[i & 1];
        unsigned char* p = static_cast<unsigned char*>(malloc(sz));
        if (!p) return 1;
        memset(p, g_pattern, sz);
        escape(p);
        g_sink += p[0];
        free(p);
        pace(i);
    }
    clock_gettime(CLOCK_MONOTONIC, &w1);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < MEASURE_ITERS; ++i) {
        size_t sz = SIZES[i & 1];
        unsigned char* p = static_cast<unsigned char*>(malloc(sz));
        if (!p) return 1;
        memset(p, g_pattern, sz);
        escape(p);
        g_sink += p[0];
        free(p);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    report("warmup:", elapsed_ns(w0, w1), WARMUP_ITERS,  ", 32/96B alternating, paced");
    report("steady:", elapsed_ns(t0, t1), MEASURE_ITERS, ", 32/96B alternating");
    return 0;
}
