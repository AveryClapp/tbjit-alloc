#include "shadow.h"
#ifdef TBJIT_SHADOW

#include <cstdlib>
#include <cassert>
#include <cstdio>

namespace tbjit::shadow {

void validate_alloc(CallSiteID id, size_t size, void* jit_ptr) {
    void* ref = std::malloc(size);
    // Size and alignment checks against jit_ptr
    // TODO: compare alignment, record (id, ref, jit_ptr) pair for free validation
    assert(jit_ptr != nullptr && "JIT'd alloc returned null");
    assert((reinterpret_cast<uintptr_t>(jit_ptr) % alignof(max_align_t)) == 0
           && "JIT'd alloc misaligned");
    std::free(ref);
    (void)id;
}

void validate_free(CallSiteID id, void* ptr) {
    // TODO: verify ptr is a known JIT'd allocation
    (void)id;
    (void)ptr;
}

} // namespace tbjit::shadow
#endif // TBJIT_SHADOW
