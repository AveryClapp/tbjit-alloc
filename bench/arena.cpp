// Arena pattern: allocate N pointers in a batch, free all of them, repeat.
// Mimics request-scoped allocations (web server handling one request,
// parser building one AST, etc). EpochArena should win big here because
// it can recycle the region every cycle without ever deopting.

#include "bench_common.h"
#include <cstdlib>
#include <cstring>

int main() {
    constexpr int    WARMUP_CYCLES  = 100;
    constexpr int    MEASURE_CYCLES = 5'000;
    constexpr int    BATCH          = 200;
    constexpr size_t SIZE           = 48;

    void* batch[BATCH];

    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    for (int c = 0; c < WARMUP_CYCLES; ++c) {
        for (int i = 0; i < BATCH; ++i) {
            unsigned char* p = static_cast<unsigned char*>(malloc(SIZE));
            if (!p) return 1;
            memset(p, g_pattern, SIZE);
            escape(p);
            g_sink += p[0];
            batch[i] = p;
        }
        for (int i = 0; i < BATCH; ++i) free(batch[i]);
        pace(c);
    }
    clock_gettime(CLOCK_MONOTONIC, &w1);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int c = 0; c < MEASURE_CYCLES; ++c) {
        for (int i = 0; i < BATCH; ++i) {
            unsigned char* p = static_cast<unsigned char*>(malloc(SIZE));
            if (!p) return 1;
            memset(p, g_pattern, SIZE);
            escape(p);
            g_sink += p[0];
            batch[i] = p;
        }
        for (int i = 0; i < BATCH; ++i) free(batch[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    report("warmup:",  elapsed_ns(w0, w1), WARMUP_CYCLES  * BATCH, ", 48B batches of 200, paced");
    report("steady:",  elapsed_ns(t0, t1), MEASURE_CYCLES * BATCH, ", 48B batches of 200");
    return 0;
}
