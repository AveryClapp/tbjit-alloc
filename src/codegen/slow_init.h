#pragma once
#include <cstddef>
#include <cstdint>

namespace tbjit::codegen {

constexpr size_t BUMP_REGION_SIZE     = 256 * 1024;
constexpr size_t FREELIST_REGION_SIZE = 256 * 1024;
constexpr size_t ARENA_REGION_SIZE    = 256 * 1024;

// Called from emitted BumpAlloc slow path. mmaps a fresh region, installs
// it into tl_bumps[index], registers the region for free-path lookup, and
// returns the first allocation pointer.
uint8_t* bump_slow_init(uint32_t index, uint32_t size);

// Called from emitted FreeListAlloc refill path. mmaps a fresh region,
// chops it into chunks of obj_size, links them into a free list installed
// at tl_freelists[index], registers the region, and pops one chunk to
// return to the caller.
void* freelist_refill(uint32_t index, uint32_t obj_size);

// Multi-class refill: like freelist_refill but for class `class_idx` of
// tl_multi_freelists[slot]. Allocates a segment dedicated to this class,
// chops into obj_size chunks, installs head at tl_multi_freelists[slot]
// .heads[class_idx], and returns one chunk.
void* multi_refill(uint32_t slot, uint32_t obj_size, uint32_t class_idx);

// First-time init for an EpochArena slot: mmaps a region, installs
// {base, end, base+size} into tl_arenas[index], registers the region,
// and returns the first allocation pointer.
uint8_t* arena_slow_init(uint32_t index, uint32_t size);

// Called from the EpochArena slow path when the bump pointer reaches end.
// Resets tl_arenas[index].ptr to base, then bumps it by size and returns
// the original base — i.e. recycles the existing region without unmapping.
// Pointers handed out before the reset are now invalid (this is the
// strategy's contract: arena allocations are scoped to an epoch and the
// user is responsible for not retaining them across resets).
uint8_t* arena_reset_alloc(uint32_t index, uint32_t size);

} // namespace tbjit::codegen
