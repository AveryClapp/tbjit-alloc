#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include "seg/segment.h"
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

// Monomorphic + churn (every alloc paired with a free) → TLFreeList, not
// BumpAlloc. BumpAlloc would exhaust its segment after ~43k allocs and
// deopt; after DEOPT_BLACKLIST_LIMIT cycles the site is blacklisted and
// every subsequent alloc falls back to glibc.
static void test_monomorphic_reap_picks_freelist() {
    analysis::reset_state();

    // Allocate a managed segment so free events have a real seg::of()
    // target. Frees are seg-managed → don't update top_pair (Bug 1 fix)
    // and don't trigger paired_pattern → picker reaches the monomorphic
    // branch with lifetime=Reap.
    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::ThreadLocalFreeList, /*slot=*/0, /*site=*/44, /*chunk_size=*/48);
    CHECK(s);
    void* fake = seg::payload_start(s);

    for (int i = 0; i < 11'000; ++i) {
        AllocEvent a{44, 48, 0, 0, nullptr};
        analysis::process_event(a);
        if (i % 10 < 7) {  // 70% free rate → Reap
            AllocEvent f{99, 0, 0, 0, fake};
            analysis::process_event(f);
        }
    }

    CHECK(analysis::get_phase(44) == analysis::Phase::Compiled);
    CHECK(analysis::get_lifetime_tag(44) == analysis::LifetimeTag::Reap);
    CHECK(analysis::get_candidate_strategy(44) ==
          Strategy::ThreadLocalFreeList);

    seg::free_segment(s);
}

// Sub-pointer-width monomorphic + churn falls back to BumpAlloc because
// TLFreeList needs space for a `next` pointer in each freed chunk.
static void test_small_monomorphic_reap_falls_back_to_bump() {
    analysis::reset_state();

    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::BumpAlloc, 0, 45, 4);
    CHECK(s);
    void* fake = seg::payload_start(s);

    for (int i = 0; i < 11'000; ++i) {
        AllocEvent a{45, 4, 0, 0, nullptr};
        analysis::process_event(a);
        if (i % 10 < 7) {
            AllocEvent f{99, 0, 0, 0, fake};
            analysis::process_event(f);
        }
    }

    CHECK(analysis::get_phase(45) == analysis::Phase::Compiled);
    CHECK(analysis::get_lifetime_tag(45) == analysis::LifetimeTag::Reap);
    CHECK(analysis::get_candidate_strategy(45) == Strategy::BumpAlloc);

    seg::free_segment(s);
}

int main() {
    test_bimodal_picks_multi();
    test_monomorphic_still_bump();
    test_monomorphic_reap_picks_freelist();
    test_small_monomorphic_reap_falls_back_to_bump();
    std::puts("test_multi_class_picker OK");
    return 0;
}
