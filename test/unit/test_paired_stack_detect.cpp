#include "analysis/analysis.h"
#include "alloc/alloc.h"
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

int main() {
    test_strict_lifo_picks_paired();
    test_diffuse_free_sites_no_paired();
    std::puts("test_paired_stack_detect OK");
    return 0;
}
