#include "analysis/analysis.h"
#include "alloc/alloc.h"
#include "seg/segment.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit;

// Allocs all from TID=100, frees all from TID=200 (via a managed segment).
// Should detect producer-consumer pattern → candidate = ProducerConsumer.
static void test_pc_pattern_picks_producer_consumer_strategy() {
    alloc::init();
    analysis::init_state();

    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::ThreadLocalFreeList, /*slot=*/0, /*site=*/7, /*chunk_size=*/48);
    CHECK(s);
    void* fake_chunk = seg::payload_start(s);

    for (int i = 0; i < 11'000; ++i) {
        AllocEvent ev_alloc{7, 48, 0, /*tid=*/100, nullptr};
        analysis::process_event(ev_alloc);
        AllocEvent ev_free{99, 0, 0, /*tid=*/200, fake_chunk};
        analysis::process_event(ev_free);
    }

    CHECK(analysis::get_phase(7) == analysis::Phase::Compiled);
    CHECK(analysis::get_candidate_strategy(7) == Strategy::ProducerConsumer);

    seg::free_segment(s);
}

// Single-thread allocs+frees on TID=100 — no PC pattern, should pick
// TLFreeList (non-monomorphic) or BumpAlloc (if monomorphic).
static void test_same_thread_no_pc_pattern() {
    analysis::reset_state();

    seg::SegmentHeader* s = seg::alloc_segment(
        Strategy::ThreadLocalFreeList, 1, 8, 48);
    CHECK(s);
    void* fake_chunk = seg::payload_start(s);

    for (int i = 0; i < 11'000; ++i) {
        AllocEvent ev_alloc{8, 48, 0, 100, nullptr};
        analysis::process_event(ev_alloc);
        AllocEvent ev_free{99, 0, 0, 100, fake_chunk};
        analysis::process_event(ev_free);
    }

    CHECK(analysis::get_phase(8) == analysis::Phase::Compiled);
    CHECK(analysis::get_candidate_strategy(8) != Strategy::ProducerConsumer);

    seg::free_segment(s);
}

int main() {
    test_pc_pattern_picks_producer_consumer_strategy();
    test_same_thread_no_pc_pattern();
    std::puts("test_producer_consumer OK");
    return 0;
}
