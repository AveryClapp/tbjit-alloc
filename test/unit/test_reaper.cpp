#include "seg/segment.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit;
using namespace tbjit::seg;

static bool always_reap(CallSiteID) { return true; }
static bool never_reap(CallSiteID)  { return false; }

static void test_retired_zero_live_eligible() {
    SegmentHeader* s = alloc_segment(Strategy::ThreadLocalFreeList, 0, 11, 48);
    CHECK(s);
    s->retired = true;
    s->live_chunks.store(0);

    CHECK(is_managed(s));
    size_t n = reaper_sweep(always_reap);
    CHECK(n >= 1);
    CHECK(!is_managed(s));
}

static void test_active_not_eligible() {
    SegmentHeader* s = alloc_segment(Strategy::ThreadLocalFreeList, 1, 22, 48);
    CHECK(s);
    // s->retired == false by default
    s->live_chunks.store(0);
    size_t n = reaper_sweep(always_reap);
    CHECK(n == 0);
    CHECK(is_managed(s));
    free_segment(s);
}

static void test_live_chunks_blocks_reclaim() {
    SegmentHeader* s = alloc_segment(Strategy::ThreadLocalFreeList, 2, 33, 48);
    CHECK(s);
    s->retired = true;
    s->live_chunks.store(5);

    size_t n = reaper_sweep(always_reap);
    CHECK(n == 0);
    CHECK(is_managed(s));

    s->live_chunks.store(0);
    n = reaper_sweep(always_reap);
    CHECK(n >= 1);
    CHECK(!is_managed(s));
}

static void test_predicate_blocks_reclaim() {
    SegmentHeader* s = alloc_segment(Strategy::ThreadLocalFreeList, 3, 44, 48);
    CHECK(s);
    s->retired = true;
    s->live_chunks.store(0);

    size_t n = reaper_sweep(never_reap);
    CHECK(n == 0);
    CHECK(is_managed(s));

    n = reaper_sweep(always_reap);
    CHECK(n >= 1);
    CHECK(!is_managed(s));
}

static void test_decrement_via_atomic_to_zero() {
    SegmentHeader* s = alloc_segment(Strategy::ThreadLocalFreeList, 4, 55, 48);
    CHECK(s);
    s->retired = true;
    s->live_chunks.store(3);

    s->live_chunks.fetch_sub(1);
    s->live_chunks.fetch_sub(1);
    CHECK(reaper_sweep(always_reap) == 0);  // still > 0

    s->live_chunks.fetch_sub(1);
    CHECK(reaper_sweep(always_reap) >= 1);
    CHECK(!is_managed(s));
}

int main() {
    test_retired_zero_live_eligible();
    test_active_not_eligible();
    test_live_chunks_blocks_reclaim();
    test_predicate_blocks_reclaim();
    test_decrement_via_atomic_to_zero();
    std::puts("test_reaper OK");
    return 0;
}
