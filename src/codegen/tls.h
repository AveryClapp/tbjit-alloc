#pragma once
#include "common.h"
#include <cstddef>
#include <cstdint>

namespace tbjit::codegen {

constexpr size_t MAX_COMPILED_SITES = 4096;

// Maps slot_index → CallSiteID. Written once when a slot is assigned for a
// specific (site, strategy) compile, read by slow_init.cpp when populating
// the segment header. Lock-free, write-once per slot.
extern CallSiteID g_slot_to_site[MAX_COMPILED_SITES];

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
