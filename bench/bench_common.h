#pragma once
#include <cstdio>
#include <ctime>

// Force the compiler to treat p as escaped: under -O2 it otherwise proves
// that p never leaves the function and folds malloc+memset+read+free into
// a single increment of the sink. The asm clobber pretends some external
// code reads from p, defeating the dead-allocation analysis.
static inline void escape(void* p) {
    asm volatile("" : : "r"(p) : "memory");
}

// Volatile pattern keeps memset's value opaque so the loop body can't be
// constant-folded back to a known result.
static volatile unsigned char g_pattern = 0xAB;
static volatile unsigned long g_sink;

static inline double elapsed_ns(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
}

// Periodic 250us sleep — gives tbjit's analysis thread room to keep up
// with the producer. Negligible cost when not running under LD_PRELOAD.
static inline void pace(int i) {
    if ((i & 1023) == 1023) {
        struct timespec ts{0, 250'000L};
        nanosleep(&ts, nullptr);
    }
}

static inline void report(const char* label, double ns, int iters, const char* extra) {
    printf("%-10s %8.1f ns/op  (%d iters%s)\n",
           label, ns / iters, iters, extra ? extra : "");
}
