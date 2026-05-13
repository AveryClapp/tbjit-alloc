#pragma once
#include <cstddef>
#include <cstdint>

namespace tbjit::codegen {

// Emits the BumpAlloc routine into buf. Returns bytes written, or 0 on error.
// buf must be at least 200 bytes.
// Calling convention: rdi = requested size, returns ptr in rax.
// tls_ptr_offset / tls_end_offset: fs-relative byte offsets (may be negative-wrapped uint32_t on Linux x86-64).
// slot_index: index into tl_bumps[], passed as arg0 to bump_slow_init.
size_t emit_bump_alloc(uint8_t* buf, size_t buf_size,
                       uint32_t tls_ptr_offset, uint32_t tls_end_offset,
                       uint32_t slot_index,
                       uint32_t dominant_size, uint32_t call_site_id,
                       void* deopt_handler, void* slow_init_fn,
                       void* real_malloc);

// Emits the FreeListAlloc routine into buf. Returns bytes written, or 0
// on error. buf must be at least 200 bytes.
// Calling convention: rdi = requested size, returns ptr in rax.
// tls_head_offset: fs-relative byte offset of tl_freelists[slot].head.
// slot_index: passed as arg0 to refill_fn when head is null.
size_t emit_freelist_alloc(uint8_t* buf, size_t buf_size,
                           uint32_t tls_head_offset,
                           uint32_t slot_index,
                           uint32_t dominant_size, uint32_t call_site_id,
                           void* deopt_handler, void* refill_fn,
                           void* real_malloc);

// Emits the EpochArena routine. Structurally close to BumpAlloc but
// instead of deopting on region-exhaust the slow path calls
// reset_alloc_fn which recycles the region in place.
// tls_ptr_offset / tls_end_offset: fs-relative offsets of
// tl_arenas[slot].ptr and .end respectively.
size_t emit_epoch_arena(uint8_t* buf, size_t buf_size,
                        uint32_t tls_ptr_offset, uint32_t tls_end_offset,
                        uint32_t slot_index,
                        uint32_t dominant_size, uint32_t call_site_id,
                        void* deopt_handler, void* reset_alloc_fn,
                        void* real_malloc);

// Emits a MultiSizeFreeList routine: a per-site branch ladder over up to 4
// learned size classes. For each class i, generates a `cmp rdi, class_sizes[i]
// / je .class_i_path` test; falls through to .deopt if no class matches.
// Each class path is a TLFreeList-style pop from heads[i], with a per-class
// refill call on empty. tls_heads_base_offset is the fs-relative offset of
// tl_multi_freelists[slot].heads[0]; heads[i] is at base + i*8.
// refill_fn signature: void* multi_refill(uint32_t slot, uint32_t size,
//                                        uint32_t class_idx).
size_t emit_multi_freelist_alloc(uint8_t* buf, size_t buf_size,
                                 uint32_t tls_heads_base_offset,
                                 uint32_t slot_index,
                                 const uint32_t* class_sizes, size_t class_count,
                                 uint32_t call_site_id,
                                 void* deopt_handler, void* refill_fn,
                                 void* real_malloc);

} // namespace tbjit::codegen
