#include "emitter.h"
#include <cstring>

namespace tbjit::codegen {

namespace {

uint8_t* w8(uint8_t* p, uint8_t v)   { *p++ = v; return p; }
uint8_t* w32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); return p + 4; }
uint8_t* w64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); return p + 8; }

} // namespace

//
// Routine layout:
//
// [fast path]
//   cmp rdi, dominant_size       ; guard: wrong size?
//   jne .deopt                   ; (rel8, patched later)
//   mov rax, fs:[tls_ptr_offset] ; load bump ptr
//   lea rcx, [rax + dominant_size] ; advance
//   cmp rcx, fs:[tls_end_offset] ; bounds check
//   jae .slow                    ; (rel8, patched later)
//   mov fs:[tls_ptr_offset], rcx ; commit new ptr
//   ret
//
// [.slow]  <-- jae patches here
//   cmp rax, 0                   ; ptr == 0 → uninitialized
//   jne .deopt                   ; (rel8, patched later)
// [.init]
//   mov edi, slot_index          ; arg0: slot index
//   movabs rax, slow_init_fn
//   call rax                     ; returns base ptr in rax
//   ret
//
// [.deopt]  <-- both jne patches here
//   push rdi                     ; save requested size
//   mov edi, call_site_id        ; arg0: call_site_id
//   movabs rsi, buf              ; arg1: code page (buf)
//   movabs rax, deopt_handler
//   call rax
//   pop rdi                      ; restore requested size
//   movabs rax, real_malloc
//   jmp rax                      ; tail-call real_malloc(size)
//

size_t emit_bump_alloc(uint8_t* buf, size_t buf_size,
                       uint32_t tls_ptr_offset, uint32_t tls_end_offset,
                       uint32_t dominant_size, uint32_t call_site_id,
                       void* deopt_handler, void* slow_init_fn,
                       void* real_malloc) {
    if (!buf || buf_size < 200) return 0;

    uint8_t* p = buf;

    // --- fast path ---

    // cmp rdi, imm32  (REX.W + 81 /7 + imm32)
    //   48 81 FF <imm32>
    p = w8(p, 0x48);
    p = w8(p, 0x81);
    p = w8(p, 0xFF);
    p = w32(p, dominant_size);

    // jne .deopt (placeholder rel8)
    //   75 <rel8>
    p = w8(p, 0x75);
    uint8_t* jne_deopt1_offset = p;  // address of rel8 byte
    p = w8(p, 0x00);                 // placeholder

    // mov rax, QWORD PTR fs:[tls_ptr_offset]
    //   64 48 8B 04 25 <imm32>
    p = w8(p, 0x64);
    p = w8(p, 0x48);
    p = w8(p, 0x8B);
    p = w8(p, 0x04);
    p = w8(p, 0x25);
    p = w32(p, tls_ptr_offset);

    // lea rcx, [rax + dominant_size]
    if (dominant_size < 128) {
        // 48 8D 48 <imm8>
        p = w8(p, 0x48);
        p = w8(p, 0x8D);
        p = w8(p, 0x48);
        p = w8(p, static_cast<uint8_t>(dominant_size));
    } else {
        // 48 8D 88 <imm32>
        p = w8(p, 0x48);
        p = w8(p, 0x8D);
        p = w8(p, 0x88);
        p = w32(p, dominant_size);
    }

    // cmp rcx, QWORD PTR fs:[tls_end_offset]
    //   64 48 3B 0C 25 <imm32>
    p = w8(p, 0x64);
    p = w8(p, 0x48);
    p = w8(p, 0x3B);
    p = w8(p, 0x0C);
    p = w8(p, 0x25);
    p = w32(p, tls_end_offset);

    // jae .slow (placeholder rel8)
    //   73 <rel8>
    p = w8(p, 0x73);
    uint8_t* jae_slow_offset = p;  // address of rel8 byte
    p = w8(p, 0x00);               // placeholder

    // mov QWORD PTR fs:[tls_ptr_offset], rcx
    //   64 48 89 0C 25 <imm32>
    p = w8(p, 0x64);
    p = w8(p, 0x48);
    p = w8(p, 0x89);
    p = w8(p, 0x0C);
    p = w8(p, 0x25);
    p = w32(p, tls_ptr_offset);

    // ret
    p = w8(p, 0xC3);

    // --- .slow label ---
    // Patch jae .slow: displacement = slow_label - (jae_slow_offset + 1)
    uint8_t* slow_label = p;
    *jae_slow_offset = static_cast<uint8_t>(slow_label - (jae_slow_offset + 1));

    // cmp rax, 0  (REX.W + 83 /7 + 00)
    //   48 83 F8 00
    p = w8(p, 0x48);
    p = w8(p, 0x83);
    p = w8(p, 0xF8);
    p = w8(p, 0x00);

    // jne .deopt (placeholder rel8)
    //   75 <rel8>
    p = w8(p, 0x75);
    uint8_t* jne_deopt2_offset = p;  // address of rel8 byte
    p = w8(p, 0x00);                 // placeholder

    // --- .init label (falls through from .slow when rax==0) ---

    // mov edi, slot_index  (BF <imm32>)
    // slot_index = tls_ptr_offset / sizeof(BumpSlot) = tls_ptr_offset / 16
    uint32_t slot_index = tls_ptr_offset / 16;
    p = w8(p, 0xBF);
    p = w32(p, slot_index);

    // mov esi, dominant_size  (BE <imm32>)
    // bump_slow_init(uint32_t index, uint32_t size) — size is the second arg
    p = w8(p, 0xBE);
    p = w32(p, dominant_size);

    // movabs rax, slow_init_fn  (48 B8 <imm64>)
    p = w8(p, 0x48);
    p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(slow_init_fn));

    // call rax  (FF D0)
    p = w8(p, 0xFF);
    p = w8(p, 0xD0);

    // ret
    p = w8(p, 0xC3);

    // --- .deopt label ---
    // Patch both jne .deopt branches
    uint8_t* deopt_label = p;
    *jne_deopt1_offset = static_cast<uint8_t>(deopt_label - (jne_deopt1_offset + 1));
    *jne_deopt2_offset = static_cast<uint8_t>(deopt_label - (jne_deopt2_offset + 1));

    // push rdi  (57)
    p = w8(p, 0x57);

    // mov edi, call_site_id  (BF <imm32>)
    p = w8(p, 0xBF);
    p = w32(p, call_site_id);

    // movabs rsi, buf  (48 BE <imm64>)
    p = w8(p, 0x48);
    p = w8(p, 0xBE);
    p = w64(p, reinterpret_cast<uint64_t>(buf));

    // movabs rax, deopt_handler  (48 B8 <imm64>)
    p = w8(p, 0x48);
    p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(deopt_handler));

    // call rax  (FF D0)
    p = w8(p, 0xFF);
    p = w8(p, 0xD0);

    // pop rdi  (5F)
    p = w8(p, 0x5F);

    // movabs rax, real_malloc  (48 B8 <imm64>)
    p = w8(p, 0x48);
    p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(real_malloc));

    // jmp rax  (FF E0)
    p = w8(p, 0xFF);
    p = w8(p, 0xE0);

    return static_cast<size_t>(p - buf);
}

} // namespace tbjit::codegen
