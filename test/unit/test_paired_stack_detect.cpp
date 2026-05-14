#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include "seg/segment.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit;

// Nested alloc/free at site S1 (allocs) and S2 (frees) in strict LIFO order
// over many repetitions → should pick PairedStack.
static void test_strict_lifo_picks_paired() {
    alloc::init();
    analysis::init_state();

    constexpr CallSiteID ALLOC_S = 51;
    constexpr CallSiteID FREE_S  = 52;

    // 11k pairs of (alloc, free). Each pair pushes a unique ptr then frees it.
    // Stack depth stays at 1 throughout — degenerate LIFO but passes the rule.
    uintptr_t base = 0x10000000;
    for (int i = 0; i < 11'000; ++i) {
        void* p = reinterpret_cast<void*>(base + i * 64);
        AllocEvent a{ALLOC_S, 48, 0, 0, p};
        analysis::process_event(a);
        AllocEvent f{FREE_S, 0, 0, 0, p};
        analysis::process_event(f);
    }

    CHECK(analysis::get_phase(ALLOC_S) == analysis::Phase::Compiled);
    CHECK(analysis::get_candidate_strategy(ALLOC_S) == Strategy::PairedStack);
}

// Frees come from many different sites — no dominant pair → not paired.
static void test_diffuse_free_sites_no_paired() {
    analysis::reset_state();

    uintptr_t base = 0x20000000;
    for (int i = 0; i < 11'000; ++i) {
        void* p = reinterpret_cast<void*>(base + i * 64);
        AllocEvent a{53, 48, 0, 0, p};
        analysis::process_event(a);
        AllocEvent f{static_cast<CallSiteID>(60 + (i % 50)), 0, 0, 0, p};
        analysis::process_event(f);
    }

    CHECK(analysis::get_phase(53) == analysis::Phase::Compiled);
    CHECK(analysis::get_candidate_strategy(53) != Strategy::PairedStack);
}

// Frees of seg-managed (JIT'd) chunks shouldn't desync pair_count from
// lifo_count. Pre-fix: JIT'd allocs never push lifo_stack, so their frees
// grew pair_count without a matching lifo_count, dragging the
// lifo_count/pair_count ratio below 0.95 and forcing the picker out of
// PairedStack on every cycle after the first compile.
static void test_jit_frees_dont_skew_paired() {
    analysis::reset_state();

    constexpr CallSiteID ALLOC_S = 71;
    constexpr CallSiteID FREE_S  = 72;

    // 1k strict-LIFO pairs through the generic path.
    uintptr_t base = 0x30000000;
    for (int i = 0; i < 1'000; ++i) {
        void* p = reinterpret_cast<void*>(base + i * 64);
        AllocEvent a{ALLOC_S, 48, 0, 0, p};
        analysis::process_event(a);
        AllocEvent f{FREE_S, 0, 0, 0, p};
        analysis::process_event(f);
    }

    // Now simulate a batch of seg-managed frees (as if a JIT'd cycle ran
    // and its chunks were freed). Without the fix, these would bump
    // pair_count by 500 with no corresponding lifo_count growth.
    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::BumpAlloc, /*slot=*/0, ALLOC_S, /*chunk_size=*/48);
    CHECK(s);
    uint8_t* payload = seg::payload_start(s);
    for (int i = 0; i < 500; ++i) {
        AllocEvent f{FREE_S, 0, 0, 0, payload + i * 48};
        analysis::process_event(f);
    }

    // Resume strict-LIFO pairs until specialization.
    for (int i = 1'000; i < 11'000; ++i) {
        void* p = reinterpret_cast<void*>(base + i * 64);
        AllocEvent a{ALLOC_S, 48, 0, 0, p};
        analysis::process_event(a);
        AllocEvent f{FREE_S, 0, 0, 0, p};
        analysis::process_event(f);
    }

    CHECK(analysis::get_phase(ALLOC_S) == analysis::Phase::Compiled);
    CHECK(analysis::get_candidate_strategy(ALLOC_S) == Strategy::PairedStack);

    seg::free_segment(s);
}

int main() {
    test_strict_lifo_picks_paired();
    test_diffuse_free_sites_no_paired();
    test_jit_frees_dont_skew_paired();
    std::puts("test_paired_stack_detect OK");
    return 0;
}
