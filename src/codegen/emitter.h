#pragma once
#include <cstddef>
#include <cstdint>

namespace tbjit::codegen {

// Emits the BumpAlloc routine into buf. Returns bytes written, or 0 on error.
// buf must be at least 128 bytes (200 recommended to be safe).
// Calling convention: rdi = requested size, returns ptr in rax.
// tls_ptr_offset = index * sizeof(BumpSlot)      (byte offset into fs: segment)
// tls_end_offset = index * sizeof(BumpSlot) + 8
size_t emit_bump_alloc(uint8_t* buf, size_t buf_size,
                       uint32_t tls_ptr_offset, uint32_t tls_end_offset,
                       uint32_t dominant_size, uint32_t call_site_id,
                       void* deopt_handler, void* slow_init_fn,
                       void* real_malloc);

} // namespace tbjit::codegen
