#pragma once
#include "../common.h"
#include <cstddef>

namespace tbjit::codegen {

struct RoutineSpec {
    CallSiteID id;
    Strategy   strategy;
    uint32_t   size;        // dominant allocation size (BumpAlloc/PairedStack)
};

// Returns a pointer to the emitted routine, or nullptr on failure.
void* compile(const RoutineSpec& spec);
void  reclaim(void* code_page);  // called after epoch safe-point

} // namespace tbjit::codegen
