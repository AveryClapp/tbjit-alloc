#include "emitter.h"
#include "analysis/analysis.h"
#include <cstring>

namespace tbjit::codegen {

namespace {

uint8_t* w8(uint8_t* p, uint8_t v)   { *p++ = v; return p; }
uint8_t* w32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); return p + 4; }
uint8_t* w64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); return p + 8; }

// Emits `mov edx, <reason>` — the ground-truth deopt reason passed as the 3rd
// arg (edx) to deopt::handle. Always on the cold deopt epilogue, so no hot-path
// cost. edx is dead on every fast path, so clobbering it here is safe.
uint8_t* w_reason(uint8_t* p, tbjit::analysis::DeoptReason r) {
    p = w8(p, 0xBA);  // mov edx, imm32
    return w32(p, static_cast<uint32_t>(r));
}

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
//   sub rsp, 8                   ; realign stack to 16 before external call
//   mov edi, slot_index          ; arg0: slot index
//   mov esi, dominant_size       ; arg1: size
//   movabs rax, slow_init_fn
//   call rax                     ; returns base ptr in rax
//   add rsp, 8                   ; undo realignment
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
                       uint32_t slot_index,
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

    // sub rsp, 8  (48 83 EC 08) — realign to 16 before external call: the
    // JIT page is entered via `call` (RSP mod 16 == 8) and does no stack
    // adjustment, so an inner `call` would land in the callee at
    // RSP mod 16 == 0, violating the SysV ABI.
    p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xEC); p = w8(p, 0x08);

    // mov edi, slot_index  (BF <imm32>)
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

    // add rsp, 8  (48 83 C4 08) — undo the realignment before ret.
    p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xC4); p = w8(p, 0x08);

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

    // mov edx, reason — BumpAlloc deopts on segment exhaust (size guard is the
    // primary, exhaust the secondary cause; bump sites are size-monomorphic by
    // selection so exhaust dominates blacklisting). Sustained size drift is
    // still recorded precisely by check_postspec.
    p = w_reason(p, tbjit::analysis::DeoptReason::RegionExhaust);

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

//
// FreeListAlloc routine layout:
//
// [fast path]
//   cmp rdi, dominant_size       ; guard
//   jne .deopt                   ; (rel8, patched later)
//   mov rax, fs:[tls_head_offset]; load head of free list
//   test rax, rax
//   jz   .refill                 ; (rel8, patched later)
//   mov rcx, [rax]               ; next = *head
//   mov fs:[tls_head_offset], rcx; head = next
//   ret
//
// [.refill]  <-- jz patches here
//   mov edi, slot_index
//   mov esi, dominant_size
//   movabs rax, refill_fn
//   call rax                     ; returns popped chunk in rax
//   ret
//
// [.deopt]  <-- jne patches here  (same as BumpAlloc)
//   push rdi
//   mov edi, call_site_id
//   movabs rsi, buf
//   movabs rax, deopt_handler
//   call rax
//   pop rdi
//   movabs rax, real_malloc
//   jmp rax
//

size_t emit_freelist_alloc(uint8_t* buf, size_t buf_size,
                           uint32_t tls_head_offset,
                           uint32_t slot_index,
                           uint32_t dominant_size, uint32_t call_site_id,
                           void* deopt_handler, void* refill_fn,
                           void* real_malloc) {
    if (!buf || buf_size < 200) return 0;

    uint8_t* p = buf;

    // --- fast path ---

    // cmp rdi, imm32  (REX.W + 81 /7 + imm32)
    p = w8(p, 0x48); p = w8(p, 0x81); p = w8(p, 0xFF);
    p = w32(p, dominant_size);

    // jne .deopt (placeholder rel8)
    p = w8(p, 0x75);
    uint8_t* jne_deopt_off = p;
    p = w8(p, 0x00);

    // mov rax, QWORD PTR fs:[tls_head_offset]
    //   64 48 8B 04 25 <imm32>
    p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x8B);
    p = w8(p, 0x04); p = w8(p, 0x25);
    p = w32(p, tls_head_offset);

    // test rax, rax  (48 85 C0)
    p = w8(p, 0x48); p = w8(p, 0x85); p = w8(p, 0xC0);

    // jz .refill (placeholder rel8)
    p = w8(p, 0x74);
    uint8_t* jz_refill_off = p;
    p = w8(p, 0x00);

    // mov rcx, QWORD PTR [rax]  (48 8B 08)
    p = w8(p, 0x48); p = w8(p, 0x8B); p = w8(p, 0x08);

    // mov QWORD PTR fs:[tls_head_offset], rcx
    //   64 48 89 0C 25 <imm32>
    p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x89);
    p = w8(p, 0x0C); p = w8(p, 0x25);
    p = w32(p, tls_head_offset);

    // ret
    p = w8(p, 0xC3);

    // --- .refill label ---
    uint8_t* refill_label = p;
    *jz_refill_off = static_cast<uint8_t>(refill_label - (jz_refill_off + 1));

    // sub rsp, 8 — realign to 16 before external call (see emit_bump_alloc).
    p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xEC); p = w8(p, 0x08);

    // mov edi, slot_index  (BF <imm32>)
    p = w8(p, 0xBF);
    p = w32(p, slot_index);

    // mov esi, dominant_size  (BE <imm32>)
    p = w8(p, 0xBE);
    p = w32(p, dominant_size);

    // movabs rax, refill_fn  (48 B8 <imm64>)
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(refill_fn));

    // call rax (FF D0)
    p = w8(p, 0xFF); p = w8(p, 0xD0);

    // add rsp, 8 — undo the realignment.
    p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xC4); p = w8(p, 0x08);

    // ret
    p = w8(p, 0xC3);

    // --- .deopt label ---
    uint8_t* deopt_label = p;
    *jne_deopt_off = static_cast<uint8_t>(deopt_label - (jne_deopt_off + 1));

    // push rdi (57)
    p = w8(p, 0x57);

    // mov edi, call_site_id (BF <imm32>)
    p = w8(p, 0xBF);
    p = w32(p, call_site_id);

    // movabs rsi, buf (48 BE <imm64>)
    p = w8(p, 0x48); p = w8(p, 0xBE);
    p = w64(p, reinterpret_cast<uint64_t>(buf));

    // mov edx, reason — TLFreeList's only deopt cause is the wrong-size guard;
    // exhaust refills a new segment instead of deopting.
    p = w_reason(p, tbjit::analysis::DeoptReason::SizeDrift);

    // movabs rax, deopt_handler (48 B8 <imm64>)
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(deopt_handler));

    // call rax (FF D0)
    p = w8(p, 0xFF); p = w8(p, 0xD0);

    // pop rdi (5F)
    p = w8(p, 0x5F);

    // movabs rax, real_malloc (48 B8 <imm64>)
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(real_malloc));

    // jmp rax (FF E0)
    p = w8(p, 0xFF); p = w8(p, 0xE0);

    return static_cast<size_t>(p - buf);
}

//
// EpochArena routine layout (BumpAlloc with reset-in-place on exhaust):
//
// [fast path]                   ; same as BumpAlloc
//   cmp rdi, dominant_size
//   jne .deopt
//   mov rax, fs:[tls_ptr_offset]
//   lea rcx, [rax + dominant_size]
//   cmp rcx, fs:[tls_end_offset]
//   jae .reset
//   mov fs:[tls_ptr_offset], rcx
//   ret
//
// [.reset]                       ; recycle region; never calls into deopt
//   mov edi, slot_index
//   mov esi, dominant_size
//   movabs rax, reset_alloc_fn
//   call rax
//   ret
//
// [.deopt]                       ; same as BumpAlloc — wrong-size guard only
//   push rdi
//   mov edi, call_site_id
//   movabs rsi, buf
//   movabs rax, deopt_handler
//   call rax
//   pop rdi
//   movabs rax, real_malloc
//   jmp rax
//

size_t emit_epoch_arena(uint8_t* buf, size_t buf_size,
                        uint32_t tls_ptr_offset, uint32_t tls_end_offset,
                        uint32_t slot_index,
                        uint32_t dominant_size, uint32_t call_site_id,
                        void* deopt_handler, void* reset_alloc_fn,
                        void* real_malloc) {
    if (!buf || buf_size < 200) return 0;

    uint8_t* p = buf;

    // cmp rdi, imm32
    p = w8(p, 0x48); p = w8(p, 0x81); p = w8(p, 0xFF);
    p = w32(p, dominant_size);

    // jne .deopt (placeholder rel8)
    p = w8(p, 0x75);
    uint8_t* jne_deopt_off = p;
    p = w8(p, 0x00);

    // mov rax, fs:[tls_ptr_offset]
    p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x8B);
    p = w8(p, 0x04); p = w8(p, 0x25);
    p = w32(p, tls_ptr_offset);

    // lea rcx, [rax + dominant_size]
    if (dominant_size < 128) {
        p = w8(p, 0x48); p = w8(p, 0x8D); p = w8(p, 0x48);
        p = w8(p, static_cast<uint8_t>(dominant_size));
    } else {
        p = w8(p, 0x48); p = w8(p, 0x8D); p = w8(p, 0x88);
        p = w32(p, dominant_size);
    }

    // cmp rcx, fs:[tls_end_offset]
    p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x3B);
    p = w8(p, 0x0C); p = w8(p, 0x25);
    p = w32(p, tls_end_offset);

    // jae .reset (placeholder rel8)
    p = w8(p, 0x73);
    uint8_t* jae_reset_off = p;
    p = w8(p, 0x00);

    // mov fs:[tls_ptr_offset], rcx
    p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x89);
    p = w8(p, 0x0C); p = w8(p, 0x25);
    p = w32(p, tls_ptr_offset);

    // ret
    p = w8(p, 0xC3);

    // --- .reset label ---
    uint8_t* reset_label = p;
    *jae_reset_off = static_cast<uint8_t>(reset_label - (jae_reset_off + 1));

    // sub rsp, 8 — realign to 16 before external call (see emit_bump_alloc).
    p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xEC); p = w8(p, 0x08);

    // mov edi, slot_index
    p = w8(p, 0xBF);
    p = w32(p, slot_index);

    // mov esi, dominant_size
    p = w8(p, 0xBE);
    p = w32(p, dominant_size);

    // movabs rax, reset_alloc_fn
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(reset_alloc_fn));

    // call rax
    p = w8(p, 0xFF); p = w8(p, 0xD0);

    // add rsp, 8 — undo the realignment.
    p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xC4); p = w8(p, 0x08);

    // ret
    p = w8(p, 0xC3);

    // --- .deopt label ---
    uint8_t* deopt_label = p;
    *jne_deopt_off = static_cast<uint8_t>(deopt_label - (jne_deopt_off + 1));

    p = w8(p, 0x57);
    p = w8(p, 0xBF); p = w32(p, call_site_id);
    p = w8(p, 0x48); p = w8(p, 0xBE);
    p = w64(p, reinterpret_cast<uint64_t>(buf));
    // EpochArena deopts on the wrong-size guard only (exhaust resets the arena).
    p = w_reason(p, tbjit::analysis::DeoptReason::SizeDrift);
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(deopt_handler));
    p = w8(p, 0xFF); p = w8(p, 0xD0);
    p = w8(p, 0x5F);
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(real_malloc));
    p = w8(p, 0xFF); p = w8(p, 0xE0);

    return static_cast<size_t>(p - buf);
}

//
// MultiSizeFreeList routine layout (up to 4 classes):
//
//   cmp rdi, class_sizes[0]
//   jne .skip_0
//   ;; class 0 pop:
//     mov rax, fs:[heads+0]; test rax,rax; jz .refill_0
//     mov rcx,[rax]; mov fs:[heads+0], rcx; ret
//   .refill_0:
//     mov edi, slot; mov esi, class_sizes[0]; mov edx, 0
//     movabs rax, refill_fn; call rax; ret
//   .skip_0:
//   cmp rdi, class_sizes[1]   ; (and so on for each class)
//   ...
//   .skip_{K-1}:                ; fall through to deopt
//   .deopt:                     ; standard wrong-size deopt + tail-call malloc
//
// Each class body is ~64 bytes; jne / jz displacements stay within rel8.
//

size_t emit_multi_freelist_alloc(uint8_t* buf, size_t buf_size,
                                 uint32_t tls_heads_base_offset,
                                 uint32_t slot_index,
                                 const uint32_t* class_sizes, size_t class_count,
                                 uint32_t call_site_id,
                                 void* deopt_handler, void* refill_fn,
                                 void* real_malloc) {
    if (!buf || buf_size < 512) return 0;
    if (class_count == 0 || class_count > 4) return 0;

    uint8_t* p = buf;

    for (size_t i = 0; i < class_count; ++i) {
        uint32_t head_off = tls_heads_base_offset +
                            static_cast<uint32_t>(i * sizeof(void*));

        // cmp rdi, class_sizes[i]
        p = w8(p, 0x48); p = w8(p, 0x81); p = w8(p, 0xFF);
        p = w32(p, class_sizes[i]);

        // jne .skip_i (rel8 placeholder)
        p = w8(p, 0x75);
        uint8_t* jne_skip = p;
        p = w8(p, 0x00);

        // --- class i fast path ---

        // mov rax, fs:[heads + i*8]
        p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x8B);
        p = w8(p, 0x04); p = w8(p, 0x25);
        p = w32(p, head_off);

        // test rax, rax
        p = w8(p, 0x48); p = w8(p, 0x85); p = w8(p, 0xC0);

        // jz .refill_i (rel8 placeholder)
        p = w8(p, 0x74);
        uint8_t* jz_refill = p;
        p = w8(p, 0x00);

        // mov rcx, [rax]
        p = w8(p, 0x48); p = w8(p, 0x8B); p = w8(p, 0x08);

        // mov fs:[heads + i*8], rcx
        p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x89);
        p = w8(p, 0x0C); p = w8(p, 0x25);
        p = w32(p, head_off);

        // ret
        p = w8(p, 0xC3);

        // --- .refill_i ---
        uint8_t* refill_label = p;
        *jz_refill = static_cast<uint8_t>(refill_label - (jz_refill + 1));

        // sub rsp, 8 — realign to 16 before external call (see emit_bump_alloc).
        p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xEC); p = w8(p, 0x08);
        // mov edi, slot_index
        p = w8(p, 0xBF); p = w32(p, slot_index);
        // mov esi, class_sizes[i]
        p = w8(p, 0xBE); p = w32(p, class_sizes[i]);
        // mov edx, i  (class index, third arg)
        p = w8(p, 0xBA); p = w32(p, static_cast<uint32_t>(i));
        // movabs rax, refill_fn
        p = w8(p, 0x48); p = w8(p, 0xB8);
        p = w64(p, reinterpret_cast<uint64_t>(refill_fn));
        // call rax
        p = w8(p, 0xFF); p = w8(p, 0xD0);
        // add rsp, 8 — undo the realignment.
        p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xC4); p = w8(p, 0x08);
        // ret
        p = w8(p, 0xC3);

        // .skip_i — next iteration's cmp lands here
        uint8_t* skip_label = p;
        intptr_t disp = skip_label - (jne_skip + 1);
        if (disp < -128 || disp > 127) return 0;  // grew beyond rel8 budget
        *jne_skip = static_cast<uint8_t>(disp);
    }

    // --- .deopt --- (fall through after all classes mismatched)

    // push rdi
    p = w8(p, 0x57);
    // mov edi, call_site_id
    p = w8(p, 0xBF); p = w32(p, call_site_id);
    // movabs rsi, buf
    p = w8(p, 0x48); p = w8(p, 0xBE);
    p = w64(p, reinterpret_cast<uint64_t>(buf));
    // MultiSizeFreeList deopts when a size misses all learned classes (drift).
    p = w_reason(p, tbjit::analysis::DeoptReason::SizeDrift);
    // movabs rax, deopt_handler
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(deopt_handler));
    // call rax
    p = w8(p, 0xFF); p = w8(p, 0xD0);
    // pop rdi
    p = w8(p, 0x5F);
    // movabs rax, real_malloc
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(real_malloc));
    // jmp rax
    p = w8(p, 0xFF); p = w8(p, 0xE0);

    return static_cast<size_t>(p - buf);
}

//
// ProducerConsumer routine layout: bump fast path + always-refill slow path.
// Compared to BumpAlloc, the slow path drops the `cmp rax, 0; jne .deopt`
// distinction — on exhaust we ALWAYS refill (which retires + mmaps).
//
//   cmp rdi, dominant_size       ; guard (same as bump)
//   jne .deopt
//   mov rax, fs:[tls_ptr_offset]
//   lea rcx, [rax + dominant_size]
//   cmp rcx, fs:[tls_end_offset]
//   jae .refill
//   mov fs:[tls_ptr_offset], rcx
//   ret
//
// [.refill]
//   mov edi, slot_index
//   mov esi, dominant_size
//   movabs rax, refill_fn
//   call rax
//   ret
//
// [.deopt]  (only fires on guard failure)
//   push rdi; mov edi, call_site_id; movabs rsi, buf;
//   movabs rax, deopt_handler; call rax; pop rdi;
//   movabs rax, real_malloc; jmp rax
//

size_t emit_pc_alloc(uint8_t* buf, size_t buf_size,
                     uint32_t tls_ptr_offset, uint32_t tls_end_offset,
                     uint32_t slot_index,
                     uint32_t dominant_size, uint32_t call_site_id,
                     void* deopt_handler, void* refill_fn,
                     void* real_malloc) {
    if (!buf || buf_size < 200) return 0;

    uint8_t* p = buf;

    // cmp rdi, imm32
    p = w8(p, 0x48); p = w8(p, 0x81); p = w8(p, 0xFF);
    p = w32(p, dominant_size);

    // jne .deopt (placeholder rel8)
    p = w8(p, 0x75);
    uint8_t* jne_deopt = p;
    p = w8(p, 0x00);

    // mov rax, fs:[tls_ptr_offset]
    p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x8B);
    p = w8(p, 0x04); p = w8(p, 0x25);
    p = w32(p, tls_ptr_offset);

    // lea rcx, [rax + dominant_size]
    if (dominant_size < 128) {
        p = w8(p, 0x48); p = w8(p, 0x8D); p = w8(p, 0x48);
        p = w8(p, static_cast<uint8_t>(dominant_size));
    } else {
        p = w8(p, 0x48); p = w8(p, 0x8D); p = w8(p, 0x88);
        p = w32(p, dominant_size);
    }

    // cmp rcx, fs:[tls_end_offset]
    p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x3B);
    p = w8(p, 0x0C); p = w8(p, 0x25);
    p = w32(p, tls_end_offset);

    // jae .refill (placeholder rel8)
    p = w8(p, 0x73);
    uint8_t* jae_refill = p;
    p = w8(p, 0x00);

    // mov fs:[tls_ptr_offset], rcx
    p = w8(p, 0x64); p = w8(p, 0x48); p = w8(p, 0x89);
    p = w8(p, 0x0C); p = w8(p, 0x25);
    p = w32(p, tls_ptr_offset);

    // ret
    p = w8(p, 0xC3);

    // --- .refill ---
    uint8_t* refill_label = p;
    *jae_refill = static_cast<uint8_t>(refill_label - (jae_refill + 1));

    // sub rsp, 8 — realign to 16 before external call (see emit_bump_alloc).
    p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xEC); p = w8(p, 0x08);
    // mov edi, slot_index
    p = w8(p, 0xBF); p = w32(p, slot_index);
    // mov esi, dominant_size
    p = w8(p, 0xBE); p = w32(p, dominant_size);
    // movabs rax, refill_fn
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(refill_fn));
    // call rax
    p = w8(p, 0xFF); p = w8(p, 0xD0);
    // add rsp, 8 — undo the realignment.
    p = w8(p, 0x48); p = w8(p, 0x83); p = w8(p, 0xC4); p = w8(p, 0x08);
    // ret
    p = w8(p, 0xC3);

    // --- .deopt ---
    uint8_t* deopt_label = p;
    *jne_deopt = static_cast<uint8_t>(deopt_label - (jne_deopt + 1));

    p = w8(p, 0x57);
    p = w8(p, 0xBF); p = w32(p, call_site_id);
    p = w8(p, 0x48); p = w8(p, 0xBE);
    p = w64(p, reinterpret_cast<uint64_t>(buf));
    // ProducerConsumer: bump fast path + always-refill slow path, so the only
    // deopt is the wrong-size guard.
    p = w_reason(p, tbjit::analysis::DeoptReason::SizeDrift);
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(deopt_handler));
    p = w8(p, 0xFF); p = w8(p, 0xD0);
    p = w8(p, 0x5F);
    p = w8(p, 0x48); p = w8(p, 0xB8);
    p = w64(p, reinterpret_cast<uint64_t>(real_malloc));
    p = w8(p, 0xFF); p = w8(p, 0xE0);

    return static_cast<size_t>(p - buf);
}

} // namespace tbjit::codegen
