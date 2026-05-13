#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit;

// Bimodal workload (60 % size 32, 40 % size 96) should produce
// candidate = MultiSizeFreeList.
static void test_bimodal_picks_multi() {
    alloc::init();
    analysis::init_state();

    for (int i = 0; i < 11'000; ++i) {
        uint32_t sz = (i % 10 < 6) ? 32 : 96;
        AllocEvent ev{42, sz, 0, 0, nullptr};
        analysis::process_event(ev);
    }

    CHECK(analysis::get_phase(42) == analysis::Phase::Compiled);
    CHECK(analysis::get_candidate_strategy(42) == Strategy::MultiSizeFreeList);
}

// Pure monomorphic (>=95% one size) should still pick BumpAlloc.
static void test_monomorphic_still_bump() {
    analysis::reset_state();

    for (int i = 0; i < 11'000; ++i) {
        AllocEvent ev{43, 64, 0, 0, nullptr};
        analysis::process_event(ev);
    }

    CHECK(analysis::get_phase(43) == analysis::Phase::Compiled);
    CHECK(analysis::get_candidate_strategy(43) == Strategy::BumpAlloc);
}

int main() {
    test_bimodal_picks_multi();
    test_monomorphic_still_bump();
    std::puts("test_multi_class_picker OK");
    return 0;
}
