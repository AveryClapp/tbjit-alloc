#pragma once
#include <cstddef>
#include <cstdint>

namespace tbjit::codegen {

constexpr size_t MAX_COMPILED_SITES = 4096;

struct BumpSlot {
    uint8_t* ptr;   // current bump position (null = uninitialized)
    uint8_t* end;   // end of region
};

struct FreeListSlot {
    void* head;     // head of singly-linked free list; null = empty/uninitialized
};

struct ArenaSlot {
    uint8_t* ptr;   // current bump position (null = uninitialized)
    uint8_t* end;   // end of region
    uint8_t* base;  // region base — used by reset slow path
};

// Each thread owns one tl_bumps array; index is baked into emitted code.
extern thread_local BumpSlot     tl_bumps[MAX_COMPILED_SITES];
extern thread_local FreeListSlot tl_freelists[MAX_COMPILED_SITES];
extern thread_local ArenaSlot    tl_arenas[MAX_COMPILED_SITES];

// Atomically assigns the next available slot index for a newly compiled site.
// Returns MAX_COMPILED_SITES on overflow (compile() should return nullptr).
uint32_t alloc_slot_index();

} // namespace tbjit::codegen
