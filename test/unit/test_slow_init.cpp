#include "codegen/slow_init.h"
#include "codegen/tls.h"
#include "seg/segment.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s\n", #c); std::abort(); } } while (0)

static void test_bump_slow_init_uses_segment() {
    uint32_t idx = tbjit::codegen::alloc_slot_index();
    uint8_t* p = tbjit::codegen::bump_slow_init(idx, 48);
    CHECK(p != nullptr);
    CHECK(tbjit::codegen::tl_bumps[idx].ptr == p + 48);
    // bump end now points at the segment end, not base + BUMP_REGION_SIZE.
    tbjit::seg::SegmentHeader* s = tbjit::seg::of(p);
    CHECK(tbjit::seg::is_managed(s));
    CHECK(s->strategy == tbjit::Strategy::BumpAlloc);
    CHECK(tbjit::codegen::tl_bumps[idx].end == tbjit::seg::segment_end(s));
    CHECK(p == tbjit::seg::payload_start(s));
}

int main() {
    test_bump_slow_init_uses_segment();
    return 0;
}
