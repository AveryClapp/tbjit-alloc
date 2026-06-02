#pragma once
#include <cstddef>
#include <cstdint>

namespace tbjit::seg { struct SegmentHeader; }

namespace tbjit::codegen {

constexpr size_t BUMP_REGION_SIZE     = 256 * 1024;
constexpr size_t FREELIST_REGION_SIZE = 256 * 1024;
constexpr size_t ARENA_REGION_SIZE    = 256 * 1024;

// Called from emitted BumpAlloc slow path. mmaps a fresh region, installs
// it into tl_bumps[index], registers the region for free-path lookup, and
// returns the first allocation pointer.
uint8_t* bump_slow_init(uint32_t index, uint32_t size);

// PairedStack uses the same bump alloc fast path; only the segment tag
// differs. The free trampoline's PairedStack case keys off the tag to
// rewind bump_ptr on a LIFO-matching free.
uint8_t* paired_slow_init(uint32_t index, uint32_t size);

// LIFO rewind for a PairedStack free. The JIT fast path reads and
// writes tl_bumps[seg->slot_index].ptr — NOT seg->bump_ptr — so the
// rewind must consult and update the TLS slot or it has no effect on
// subsequent JIT allocs (which is what made PairedStack effectively
// behave like BumpAlloc on churn workloads).
//
// Returns true if `ptr` was the most-recent alloc and the cursor was
// rewound; false if the free violates LIFO and the chunk is dropped.
// Caller is responsible for verifying seg->owner_tid matches the
// current thread before invoking — cross-thread frees can't safely
// touch the owner's TLS.
bool paired_lifo_rewind(seg::SegmentHeader* seg, void* ptr);

// ProducerConsumer refill: called from both initial alloc and bump-exhaust
// slow paths. If a prior segment exists in tl_bumps[slot], retires it
// (drains its MPSC remote queue first; sets live_chunks to chunks-served
// minus chunks-drained). Then mmaps a fresh ProducerConsumer-tagged
// segment, installs it, and returns the first chunk.
uint8_t* pc_refill(uint32_t index, uint32_t size);

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

// Return a tbjit-managed chunk to its strategy's free structures. `s` must be
// the (managed) segment header of `ptr` — the caller is responsible for the
// seg::is_managed(s) check. This is the strategy-switch body shared by the
// LD_PRELOAD free() trampoline and the offline bound-replay harness, both of
// which reclaim managed chunks identically but the harness runs without the
// trampoline preamble (reentrancy guard, safe-point, dispatch).
void free_managed(seg::SegmentHeader* s, void* ptr);

// Called from the EpochArena slow path when the bump pointer reaches end.
// Resets tl_arenas[index].ptr to base, then bumps it by size and returns
// the original base — i.e. recycles the existing region without unmapping.
// Pointers handed out before the reset are now invalid (this is the
// strategy's contract: arena allocations are scoped to an epoch and the
// user is responsible for not retaining them across resets).
uint8_t* arena_reset_alloc(uint32_t index, uint32_t size);

} // namespace tbjit::codegen
