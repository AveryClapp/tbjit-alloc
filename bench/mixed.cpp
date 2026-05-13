// Polymorphic: three rotating sizes at the same call site. is_monomorphic
// returns false, so the analyzer picks ThreadLocalFreeList; but TLFreeList
// has a hardcoded dominant_size and will deopt on the other two sizes,
// driving the site into the blacklist over time. This is the "JIT can't
// help" case — the benchmark exists to confirm we don't *slow it down*
// meaningfully vs glibc.

#include "bench_common.h"
#include <cstdlib>
#include <cstring>

int main() {
    constexpr int WARMUP_ITERS  = 30'000;
    constexpr int MEASURE_ITERS = 600'000;

    constexpr size_t SIZES[] = {32, 64, 128};
    constexpr int    N_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);

    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        size_t sz = SIZES[i % N_SIZES];
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
        size_t sz = SIZES[i % N_SIZES];
        unsigned char* p = static_cast<unsigned char*>(malloc(sz));
        if (!p) return 1;
        memset(p, g_pattern, sz);
        escape(p);
        g_sink += p[0];
        free(p);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    report("warmup:",  elapsed_ns(w0, w1), WARMUP_ITERS,  ", 32/64/128B rotating, paced");
    report("steady:",  elapsed_ns(t0, t1), MEASURE_ITERS, ", 32/64/128B rotating");
    return 0;
}
