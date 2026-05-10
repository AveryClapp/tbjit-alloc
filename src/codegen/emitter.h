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

} // namespace tbjit::codegen
