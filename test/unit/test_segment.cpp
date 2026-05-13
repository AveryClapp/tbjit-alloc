#include "seg/segment.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "CHECK failed: %s @ %s:%d\n",          \
                         #cond, __FILE__, __LINE__);                    \
            std::abort();                                               \
        }                                                               \
    } while (0)

using namespace tbjit;
using namespace tbjit::seg;

static void test_alignment_constants() {
    static_assert(SEGMENT_SIZE == (2ull << 20), "2 MiB");
    static_assert((SEGMENT_SIZE & (SEGMENT_SIZE - 1)) == 0, "pow of 2");
    static_assert(sizeof(SegmentHeader) == 64, "one cache line");
}

static void test_alloc_returns_aligned_segment() {
    SegmentHeader* s = alloc_segment(Strategy::BumpAlloc, 7, 42, 64);
    CHECK(s);
    CHECK((reinterpret_cast<uintptr_t>(s) & (SEGMENT_SIZE - 1)) == 0);
    CHECK(s->strategy == Strategy::BumpAlloc);
    CHECK(s->slot_index == 7);
    CHECK(s->alloc_site == 42);
    CHECK(s->chunk_size == 64);
    CHECK(s->live_chunks.load() == 0);
    CHECK(s->bump_ptr == payload_start(s));
    CHECK(s->bump_limit == segment_end(s));
    CHECK(s->next_in_site == nullptr);
    CHECK(s->remote_head.load() == nullptr);
    free_segment(s);
}

static void test_of_recovers_segment_from_payload_ptr() {
    SegmentHeader* s = alloc_segment(Strategy::ThreadLocalFreeList, 1, 0, 64);
    void* mid = payload_start(s) + 1024;
    CHECK(of(mid) == s);
    void* near_end = base_of(s) + SEGMENT_SIZE - 1;
    CHECK(of(near_end) == s);
    free_segment(s);
}

static void test_is_managed_roundtrip() {
    SegmentHeader* s = alloc_segment(Strategy::BumpAlloc, 0, 0, 64);
    CHECK(is_managed(s));
    void* mid = payload_start(s);
    CHECK(is_managed(of(mid)));
    free_segment(s);
    CHECK(!is_managed(s));
}

static void test_unmanaged_pointer_not_in_index() {
    int local = 0;
    SegmentHeader* h = of(&local);
    CHECK(!is_managed(h));
}

int main() {
    test_alignment_constants();
    test_alloc_returns_aligned_segment();
    test_of_recovers_segment_from_payload_ptr();
    test_is_managed_roundtrip();
    test_unmanaged_pointer_not_in_index();
    std::puts("test_segment OK");
    return 0;
}
