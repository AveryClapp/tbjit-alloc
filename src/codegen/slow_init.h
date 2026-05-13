#pragma once
#include <cstddef>
#include <cstdint>

namespace tbjit::codegen {

constexpr size_t BUMP_REGION_SIZE     = 256 * 1024;
constexpr size_t FREELIST_REGION_SIZE = 256 * 1024;

// Called from emitted BumpAlloc slow path. mmaps a fresh region, installs
// it into tl_bumps[index], registers the region for free-path lookup, and
// returns the first allocation pointer.
uint8_t* bump_slow_init(uint32_t index, uint32_t size);

// Called from emitted FreeListAlloc refill path. mmaps a fresh region,
// chops it into chunks of obj_size, links them into a free list installed
// at tl_freelists[index], registers the region, and pops one chunk to
// return to the caller.
void* freelist_refill(uint32_t index, uint32_t obj_size);

// Region kinds reported by find_region.
enum class RegionKind : uint8_t { Bump, FreeList };

struct RegionInfo {
    RegionKind kind;
    uint32_t   slot_index;   // valid only for FreeList
};

// True if ptr lies inside any registered region. When non-null, *info_out
// receives the kind + (for free lists) the slot index, so the free
// interceptor can dispatch to tl_freelists[slot].
bool find_region(const void* ptr, RegionInfo* info_out);

// Convenience: legacy "is this a bump region" predicate used by the
// existing free interceptor before the generalized registry landed.
// Retained so callers don't all need to update at once.
bool is_in_bump_region(const void* ptr);

} // namespace tbjit::codegen
