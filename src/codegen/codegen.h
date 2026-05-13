#pragma once
#include "../common.h"
#include <cstddef>

namespace tbjit::codegen {

struct RoutineSpec {
    CallSiteID id;
    Strategy   strategy;
    uint32_t   size;        // dominant size (BumpAlloc / PairedStack / class 0)
    uint32_t   class_sizes[4];  // MultiSizeFreeList: per-class sizes
    uint8_t    class_count;     // 0 for non-multi; 1..4 for multi
};

// Returns a pointer to the emitted routine, or nullptr on failure.
void* compile(const RoutineSpec& spec);
void  reclaim(void* code_page);  // called after epoch safe-point

} // namespace tbjit::codegen
