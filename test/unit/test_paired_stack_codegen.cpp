#include "codegen/slow_init.h"
#include "codegen/tls.h"
#include "seg/segment.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit;
using namespace tbjit::codegen;

// paired_slow_init mirrors bump_slow_init but tags the segment PairedStack
// so the free trampoline rewinds bump_ptr instead of dropping the chunk.
static void test_paired_slow_init_tags_segment() {
    uint32_t idx = alloc_slot_index();
    g_slot_to_site[idx] = 1234;
    uint8_t* p = paired_slow_init(idx, 64);
    CHECK(p);
    CHECK(tl_bumps[idx].ptr == p + 64);

    seg::SegmentHeader* s = seg::of(p);
    CHECK(seg::is_managed(s));
    CHECK(s->strategy == Strategy::PairedStack);
    CHECK(s->alloc_site == 1234);
    CHECK(s->chunk_size == 64);
    CHECK(p == seg::payload_start(s));
}

// The trampoline's LIFO check: rewound = bump_ptr - chunk_size; rewind iff
// rewound matches the freed ptr. Simulate that logic directly.
static void test_lifo_rewind_matches_top_alloc() {
    uint32_t idx = alloc_slot_index();
    g_slot_to_site[idx] = 5678;
    uint8_t* base = paired_slow_init(idx, 32);
    seg::SegmentHeader* s = seg::of(base);

    // After slow_init, one 32-byte alloc was returned (= base). bump_ptr
    // sits at base + 32. A LIFO free of `base` should rewind to base.
    uint8_t* freed = base;
    uint8_t* rewound = s->bump_ptr - s->chunk_size;
    CHECK(rewound == freed);
    s->bump_ptr = rewound;
    CHECK(s->bump_ptr == base);
}

// Out-of-order free: free a ptr that isn't bump_ptr - chunk_size → don't
// rewind (logic in the trampoline drops the chunk).
static void test_lifo_violation_does_not_rewind() {
    uint32_t idx = alloc_slot_index();
    g_slot_to_site[idx] = 9999;
    uint8_t* base = paired_slow_init(idx, 48);
    seg::SegmentHeader* s = seg::of(base);

    // Simulate 3 sequential allocs: bump_ptr moves to base + 3*48.
    s->bump_ptr = base + 3 * 48;

    // User tries to free the SECOND alloc (base + 48), not the top.
    uint8_t* freed = base + 48;
    uint8_t* rewound = s->bump_ptr - s->chunk_size;
    CHECK(rewound != freed);            // LIFO check fails
    CHECK(s->bump_ptr == base + 3 * 48); // bump unchanged
}

int main() {
    test_paired_slow_init_tags_segment();
    test_lifo_rewind_matches_top_alloc();
    test_lifo_violation_does_not_rewind();
    std::puts("test_paired_stack_codegen OK");
    return 0;
}
