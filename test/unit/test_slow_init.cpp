#include "codegen/slow_init.h"
#include "codegen/tls.h"
#include <cassert>

static void test_slow_init_returns_valid_ptr() {
    uint32_t idx = tbjit::codegen::alloc_slot_index();
    uint8_t* p = tbjit::codegen::bump_slow_init(idx, 48);
    assert(p != nullptr);
    assert(tbjit::codegen::tl_bumps[idx].ptr == p + 48);
    assert(tbjit::codegen::tl_bumps[idx].end == p + tbjit::codegen::BUMP_REGION_SIZE);
}

int main() {
    test_slow_init_returns_valid_ptr();
    return 0;
}
