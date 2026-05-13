// Pulse pattern: bursts of monomorphic allocations separated by short
// quiet periods. Models event-loop-style workloads — one event ⇒ one
// burst of allocations ⇒ idle ⇒ repeat. Important because it lets the
// analysis thread "catch up" naturally between bursts, exercising the
// futex-park / wake path in the background loop.

#include "bench_common.h"
#include <cstdlib>
#include <cstring>

int main() {
    constexpr int    WARMUP_BURSTS  = 50;
    constexpr int    MEASURE_BURSTS = 5'000;
    constexpr int    BURST          = 200;
    constexpr size_t SIZE           = 48;

    auto run_one_burst = [&]() {
        for (int i = 0; i < BURST; ++i) {
            unsigned char* p = static_cast<unsigned char*>(malloc(SIZE));
            if (!p) std::exit(1);
            memset(p, g_pattern, SIZE);
            escape(p);
            g_sink += p[0];
            free(p);
        }
    };

    struct timespec quiet{0, 500'000L};  // 500us idle between bursts

    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    for (int b = 0; b < WARMUP_BURSTS; ++b) {
        run_one_burst();
        nanosleep(&quiet, nullptr);
    }
    clock_gettime(CLOCK_MONOTONIC, &w1);

    // Measurement excludes the inter-burst idle time so the ns/op number
    // reflects actual allocation cost.
    double measure_ns = 0;
    struct timespec b0, b1;
    for (int b = 0; b < MEASURE_BURSTS; ++b) {
        clock_gettime(CLOCK_MONOTONIC, &b0);
        run_one_burst();
        clock_gettime(CLOCK_MONOTONIC, &b1);
        measure_ns += elapsed_ns(b0, b1);
        nanosleep(&quiet, nullptr);
    }

    int total_warmup  = WARMUP_BURSTS  * BURST;
    int total_measure = MEASURE_BURSTS * BURST;
    report("warmup:",  elapsed_ns(w0, w1) - WARMUP_BURSTS * 500'000.0,
           total_warmup,  ", 48B bursts of 200, 500us quiet");
    report("steady:",  measure_ns, total_measure,
           ", 48B bursts of 200, 500us quiet (excludes idle)");
    return 0;
}
