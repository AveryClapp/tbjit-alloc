// micro_jit_direct: measures the JIT alloc fast path *without* going
// through the LD_PRELOAD trampoline. Compiles a BumpAlloc routine, sets
// up its TLS slot, and calls fn(SIZE) in a tight loop directly. This
// isolates the per-call cost of the emitted code (~5 instructions: cmp,
// mov fs:[], lea, cmp fs:[], mov fs:[]) from the ~50 ns trampoline
// overhead (return-addr hash, dispatch::lookup, mark_safe_point,
// reentrancy guard, shadow::validate_alloc) that dominates the
// regular bench measurements.
//
// Paper context: in a real language-runtime integration (V8/HHVM/etc.)
// the JIT routine would be called directly, not interposed via
// LD_PRELOAD. This bench answers "what would the per-call cost look
// like in that hypothetical."
//
// Each trial:
//   1. Reset tl_bumps[slot].ptr to the segment's payload base.
//   2. Time ITERS calls of fn(SIZE) — pure fast-path bump.
//   3. After the loop, the segment is consumed but we don't care: the
//      next trial rewinds the cursor in place. No mmap thrash.
//
// We also measure glibc malloc(SIZE) on the same iteration count for a
// reference point. We do NOT free in either loop — this isolates the
// alloc cost only and avoids tcache/freelist confounders.

#include "bench_common.h"
#include <cstdio>

#if !defined(__linux__) || !defined(__x86_64__)
int main() {
    std::printf("micro_jit_direct: skipped (Linux x86-64 only — the bench "
                "calls JIT-emitted code directly).\n");
    return 0;
}
#else

#include "codegen/codegen.h"
#include "codegen/tls.h"
#include "codegen/slow_init.h"
#include "seg/segment.h"
#include "common.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>

using namespace tbjit;

namespace {

constexpr int    TRIALS = 25;
constexpr int    ITERS  = 30'000;   // < SEGMENT_SIZE/SIZE so no exhaust
constexpr size_t SIZE   = 48;

double median_of(double* samples, int n) {
    std::sort(samples, samples + n);
    return (n & 1) ? samples[n / 2]
                   : 0.5 * (samples[n / 2 - 1] + samples[n / 2]);
}

void run_jit_direct() {
    codegen::RoutineSpec spec{};
    spec.id       = 0xfeedface;
    spec.strategy = Strategy::BumpAlloc;
    spec.size     = SIZE;

    void* routine = codegen::compile(spec);
    if (!routine) {
        std::fprintf(stderr, "micro_jit_direct: compile failed\n");
        std::exit(1);
    }

    using AllocFn = void* (*)(size_t);
    AllocFn fn = reinterpret_cast<AllocFn>(routine);

    // First call drives bump_slow_init: mmaps the segment, sets up the
    // TLS slot, returns the first chunk. We capture that pointer as
    // the payload base for the rewind below.
    uint8_t* base = static_cast<uint8_t*>(fn(SIZE));
    if (!base) { std::fprintf(stderr, "fn(SIZE) returned null\n"); std::exit(1); }

    // Locate the slot we ended up in by walking back from the segment
    // header (segment_of(base) finds it; its slot_index is the TLS slot).
    seg::SegmentHeader* s = seg::of(base);
    uint32_t slot = s->slot_index;

    double samples[TRIALS];
    for (int t = 0; t < TRIALS; ++t) {
        // Rewind the TLS cursor (and the header mirror) without mmaping
        // a fresh segment.
        codegen::tl_bumps[slot].ptr = base;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < ITERS; ++i) {
            void* p = fn(SIZE);
            escape(p);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        samples[t] = elapsed_ns(t0, t1) / ITERS;
    }

    double med = median_of(samples, TRIALS);
    double lo  = samples[0], hi = samples[TRIALS - 1];
    std::printf("jit_direct: %6.2f ns/op  (median of %d trials × %d iters, "
                "range %.2f..%.2f)\n", med, TRIALS, ITERS, lo, hi);
}

void run_glibc_reference() {
    double samples[TRIALS];
    for (int t = 0; t < TRIALS; ++t) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < ITERS; ++i) {
            void* p = std::malloc(SIZE);
            escape(p);
            // Skip free intentionally: we're measuring pure alloc cost,
            // matching the JIT loop above. glibc holds ~1.4 MiB per trial
            // which it has plenty of headroom for.
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        samples[t] = elapsed_ns(t0, t1) / ITERS;
    }
    double med = median_of(samples, TRIALS);
    double lo  = samples[0], hi = samples[TRIALS - 1];
    std::printf("glibc_ref : %6.2f ns/op  (median of %d trials × %d iters, "
                "range %.2f..%.2f)\n", med, TRIALS, ITERS, lo, hi);
}

} // namespace

int main() {
    std::printf("Microbench: JIT alloc fast path vs glibc malloc, no LD_PRELOAD.\n");
    std::printf("Both loops do alloc-only (no free) on size=%zu, %d iters × %d trials.\n\n",
                SIZE, ITERS, TRIALS);
    run_jit_direct();
    run_glibc_reference();
    return 0;
}

#endif // __linux__ && __x86_64__
