#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

// Allocation throughput benchmark. Run standalone (glibc baseline) or under
// LD_PRELOAD with libtbjit (JIT path). Reports a single ns/op figure plus
// the warmup vs steady-state split.

// Force the compiler to treat p as escaped: under -O2 it otherwise proves
// that p never leaves the function and folds malloc+memset+read+free into
// a single increment of g_sink. The asm clobber pretends some external
// code reads from p, defeating the dead-allocation analysis.
static inline void escape(void* p) {
    asm volatile("" : : "r"(p) : "memory");
}

static volatile unsigned char g_pattern = 0xAB;
static volatile unsigned long g_sink;

static double elapsed_ns(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
}

int main() {
    constexpr int    WARMUP_ITERS  = 30'000;
    constexpr int    MEASURE_ITERS = 1'000'000;
    constexpr size_t SIZE          = 48;

    // Warmup: when running under tbjit, paces the producer so the analysis
    // thread can keep up and reach the stability threshold for compilation.
    // Has negligible cost without LD_PRELOAD.
    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        unsigned char* p = static_cast<unsigned char*>(malloc(SIZE));
        if (!p) return 1;
        memset(p, g_pattern, SIZE);
        escape(p);
        g_sink += p[0] + p[SIZE - 1];
        free(p);
        if ((i & 1023) == 1023) {
            struct timespec ts{0, 250'000L};
            nanosleep(&ts, nullptr);
        }
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

    double warmup_ns  = elapsed_ns(w0, w1);
    double measure_ns = elapsed_ns(t0, t1);
    printf("warmup:  %8.1f ns/op  (%d iters, %zu bytes, paced)\n",
           warmup_ns  / WARMUP_ITERS,  WARMUP_ITERS,  SIZE);
    printf("steady:  %8.1f ns/op  (%d iters, %zu bytes)\n",
           measure_ns / MEASURE_ITERS, MEASURE_ITERS, SIZE);
    return 0;
}
