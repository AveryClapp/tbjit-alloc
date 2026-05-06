#include "alloc/alloc.h"
#include <cassert>
#include <cstdint>

int main() {
    tbjit::alloc::init();

    void* p1 = tbjit::alloc::alloc(64);
    assert(p1 != nullptr);
    assert((reinterpret_cast<uintptr_t>(p1) % alignof(max_align_t)) == 0);

    void* p2 = tbjit::alloc::alloc(128, 64);
    assert(p2 != nullptr);
    assert((reinterpret_cast<uintptr_t>(p2) % 64) == 0);
    assert(p2 != p1);

    return 0;
}
