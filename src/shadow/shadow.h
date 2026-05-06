#pragma once
#include "../common.h"
#include <cstddef>

// Shadow validator: only compiled when TBJIT_SHADOW=1.
// Runs the generic allocator in parallel with the JIT'd path
// and asserts size, alignment, and free acceptance match.
namespace tbjit::shadow {

#ifdef TBJIT_SHADOW
void validate_alloc(CallSiteID id, size_t size, void* jit_ptr);
void validate_free(CallSiteID id, void* ptr);
#else
inline void validate_alloc(CallSiteID, size_t, void*) {}
inline void validate_free(CallSiteID, void*) {}
#endif

} // namespace tbjit::shadow
