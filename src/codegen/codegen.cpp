#include "codegen.h"
#include "emitter.h"
#include "tls.h"
#include "slow_init.h"
#include "deopt/deopt.h"
#include "analysis/histogram.h"
#include <sys/mman.h>
#include <cassert>

extern void* (*g_real_malloc)(size_t);

namespace tbjit::codegen {

namespace {
constexpr size_t CODE_PAGE_SIZE = 4096;

void* alloc_exec_page() {
    void* p = mmap(nullptr, CODE_PAGE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    return p;
}

void make_executable(void* page) {
    [[maybe_unused]] int r = mprotect(page, CODE_PAGE_SIZE, PROT_READ | PROT_EXEC);
    assert(r == 0);
}
} // namespace

void* compile(const RoutineSpec& spec) {
    if (spec.size == 0 || spec.size >= tbjit::analysis::ExactHistogram::MAX_SIZE) return nullptr;
    // ProducerConsumer reuses the TLFreeList codegen path. PairedStack
    // falls back to BumpAlloc — the alloc fast path is identical; the
    // LIFO-rewind free emitter and dual-site compile is future work.
    // MultiSizeFreeList has its own emitter below.
    Strategy effective = spec.strategy;
    if (effective == Strategy::ProducerConsumer)
        effective = Strategy::ThreadLocalFreeList;
    // PairedStack keeps its own slow-init (segment tagged PairedStack so the
    // free trampoline rewinds bump_ptr on a LIFO match). The alloc emitter
    // is the bump emitter — fast path is identical to BumpAlloc.
    bool paired_alloc = (effective == Strategy::PairedStack);
    if (effective == Strategy::PairedStack)
        effective = Strategy::BumpAlloc;
    if (effective != Strategy::BumpAlloc &&
        effective != Strategy::ThreadLocalFreeList &&
        effective != Strategy::EpochArena &&
        effective != Strategy::MultiSizeFreeList) return nullptr;
    if (effective == Strategy::ThreadLocalFreeList && spec.size < sizeof(void*))
        return nullptr;
    if (effective == Strategy::MultiSizeFreeList) {
        if (spec.class_count == 0 || spec.class_count > 4) return nullptr;
        for (uint8_t i = 0; i < spec.class_count; ++i)
            if (spec.class_sizes[i] < sizeof(void*)) return nullptr;
    }

    uint32_t idx = alloc_slot_index();
    if (idx >= static_cast<uint32_t>(MAX_COMPILED_SITES)) return nullptr;
    g_slot_to_site[idx] = spec.id;

    uint8_t* page = static_cast<uint8_t*>(alloc_exec_page());
    size_t n = 0;

    if (effective == Strategy::BumpAlloc) {
#if defined(__linux__) && defined(__x86_64__)
        uintptr_t tp = reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
        uint32_t ptr_off = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(&tl_bumps[idx].ptr) - tp);
        uint32_t end_off = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(&tl_bumps[idx].end) - tp);
#else
        uint32_t ptr_off = idx * static_cast<uint32_t>(sizeof(BumpSlot));
        uint32_t end_off = ptr_off + static_cast<uint32_t>(sizeof(uint8_t*));
#endif
        n = emit_bump_alloc(
            page, CODE_PAGE_SIZE,
            ptr_off, end_off,
            idx,
            spec.size, spec.id,
            reinterpret_cast<void*>(tbjit::deopt::handle),
            reinterpret_cast<void*>(paired_alloc ? paired_slow_init
                                                 : bump_slow_init),
            reinterpret_cast<void*>(g_real_malloc));
    } else if (effective == Strategy::ThreadLocalFreeList) {
#if defined(__linux__) && defined(__x86_64__)
        uintptr_t tp = reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
        uint32_t head_off = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(&tl_freelists[idx].head) - tp);
#else
        uint32_t head_off = idx * static_cast<uint32_t>(sizeof(FreeListSlot));
#endif
        n = emit_freelist_alloc(
            page, CODE_PAGE_SIZE,
            head_off,
            idx,
            spec.size, spec.id,
            reinterpret_cast<void*>(tbjit::deopt::handle),
            reinterpret_cast<void*>(freelist_refill),
            reinterpret_cast<void*>(g_real_malloc));
    } else if (effective == Strategy::MultiSizeFreeList) {
#if defined(__linux__) && defined(__x86_64__)
        uintptr_t tp = reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
        uint32_t heads_off = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(&tl_multi_freelists[idx].heads[0]) - tp);
#else
        uint32_t heads_off = idx * static_cast<uint32_t>(sizeof(MultiFreeListSlot));
#endif
        n = emit_multi_freelist_alloc(
            page, CODE_PAGE_SIZE,
            heads_off,
            idx,
            spec.class_sizes, spec.class_count,
            spec.id,
            reinterpret_cast<void*>(tbjit::deopt::handle),
            reinterpret_cast<void*>(multi_refill),
            reinterpret_cast<void*>(g_real_malloc));
    } else { // EpochArena
#if defined(__linux__) && defined(__x86_64__)
        uintptr_t tp = reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
        uint32_t ptr_off = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(&tl_arenas[idx].ptr) - tp);
        uint32_t end_off = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(&tl_arenas[idx].end) - tp);
#else
        uint32_t ptr_off = idx * static_cast<uint32_t>(sizeof(ArenaSlot));
        uint32_t end_off = ptr_off + static_cast<uint32_t>(sizeof(uint8_t*));
#endif
        n = emit_epoch_arena(
            page, CODE_PAGE_SIZE,
            ptr_off, end_off,
            idx,
            spec.size, spec.id,
            reinterpret_cast<void*>(tbjit::deopt::handle),
            reinterpret_cast<void*>(arena_reset_alloc),
            reinterpret_cast<void*>(g_real_malloc));
    }

    if (n == 0) { munmap(page, CODE_PAGE_SIZE); return nullptr; }
    make_executable(page);
    return page;
}

void reclaim(void* code_page) {
    munmap(code_page, CODE_PAGE_SIZE);
}

} // namespace tbjit::codegen
