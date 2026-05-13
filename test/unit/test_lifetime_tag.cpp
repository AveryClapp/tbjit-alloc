#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include "seg/segment.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit;

// Stable 48-byte allocations with frequent frees → should be tagged Reap.
static void test_reap_when_frees_dominate() {
    alloc::init();
    analysis::init_state();

    // Allocate one managed segment so free events have somewhere to point.
    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::ThreadLocalFreeList, /*slot=*/0, /*site=*/1, /*chunk_size=*/48);
    CHECK(s);
    void* fake_chunk = seg::payload_start(s);

    // 11k allocs (drive site 1 to Compiled) interleaved with frees.
    for (int i = 0; i < 11'000; ++i) {
        AllocEvent ev_alloc{1, 48, 0, 0, nullptr};
        analysis::process_event(ev_alloc);
        // 70 % free rate → above LIFETIME_REAP_RATIO (0.5).
        if (i % 10 < 7) {
            AllocEvent ev_free{99, 0, 0, 0, fake_chunk};
            analysis::process_event(ev_free);
        }
    }

    CHECK(analysis::get_phase(1) == analysis::Phase::Compiled);
    CHECK(analysis::get_lifetime_tag(1) == analysis::LifetimeTag::Reap);

    seg::free_segment(s);
}

// 11k allocs, no frees → should be tagged Hold.
static void test_hold_when_no_frees() {
    analysis::reset_state();

    for (int i = 0; i < 11'000; ++i) {
        AllocEvent ev_alloc{2, 64, 0, 0, nullptr};
        analysis::process_event(ev_alloc);
    }

    CHECK(analysis::get_phase(2) == analysis::Phase::Compiled);
    CHECK(analysis::get_lifetime_tag(2) == analysis::LifetimeTag::Hold);
}

// 30 % free rate — falls between hold (0.10) and reap (0.50).
static void test_unknown_when_ratio_intermediate() {
    analysis::reset_state();

    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::ThreadLocalFreeList, 1, 3, 48);
    CHECK(s);
    void* fake_chunk = seg::payload_start(s);

    for (int i = 0; i < 11'000; ++i) {
        AllocEvent ev_alloc{3, 48, 0, 0, nullptr};
        analysis::process_event(ev_alloc);
        if (i % 10 < 3) {
            AllocEvent ev_free{99, 0, 0, 0, fake_chunk};
            analysis::process_event(ev_free);
        }
    }

    CHECK(analysis::get_phase(3) == analysis::Phase::Compiled);
    CHECK(analysis::get_lifetime_tag(3) == analysis::LifetimeTag::Unknown);

    seg::free_segment(s);
}

int main() {
    test_reap_when_frees_dominate();
    test_hold_when_no_frees();
    test_unknown_when_ratio_intermediate();
    std::puts("test_lifetime_tag OK");
    return 0;
}
