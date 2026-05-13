// LIFO pattern: distinct alloc site / free site pair, nested allocations
// freed in strict last-in-first-out order. Drives PairedStack detection
// (top_pair.lifo_count / pair_count ratio crosses the 0.95 threshold).
//
// The noinline helpers ensure malloc() and free() are reached from two
// distinct return addresses (= distinct CallSiteIDs in the trampoline).

#include "bench_common.h"
#include <cstdlib>
#include <cstring>

constexpr int    DEPTH = 8;
constexpr size_t SIZE  = 48;

__attribute__((noinline))
static unsigned char* do_alloc() {
    auto* p = static_cast<unsigned char*>(malloc(SIZE));
    if (p) memset(p, g_pattern, SIZE);
    escape(p);
    return p;
}

__attribute__((noinline))
static void do_free(unsigned char* p) {
    free(p);
}

int main() {
    constexpr int WARMUP_CYCLES  = 4'000;
    constexpr int MEASURE_CYCLES = 100'000;

    unsigned char* stack[DEPTH];

    auto one_cycle = [&]() {
        for (int d = 0; d < DEPTH; ++d) {
            stack[d] = do_alloc();
            g_sink += stack[d][0];
        }
        for (int d = DEPTH - 1; d >= 0; --d) {
            do_free(stack[d]);
        }
    };

    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    for (int c = 0; c < WARMUP_CYCLES; ++c) {
        one_cycle();
        pace(c);
    }
    clock_gettime(CLOCK_MONOTONIC, &w1);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int c = 0; c < MEASURE_CYCLES; ++c) one_cycle();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    report("warmup:", elapsed_ns(w0, w1), WARMUP_CYCLES  * DEPTH * 2,
           ", nested LIFO depth 8, paced");
    report("steady:", elapsed_ns(t0, t1), MEASURE_CYCLES * DEPTH * 2,
           ", nested LIFO depth 8");
    return 0;
}
