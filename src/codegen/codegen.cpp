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
    if (spec.strategy != Strategy::BumpAlloc &&
        spec.strategy != Strategy::ThreadLocalFreeList) return nullptr;
    // Free-list chunks need to fit a `next` pointer in their first 8 bytes.
    if (spec.strategy == Strategy::ThreadLocalFreeList && spec.size < sizeof(void*))
        return nullptr;

    uint32_t idx = alloc_slot_index();
    if (idx >= static_cast<uint32_t>(MAX_COMPILED_SITES)) return nullptr;

    uint8_t* page = static_cast<uint8_t*>(alloc_exec_page());
    size_t n = 0;

    if (spec.strategy == Strategy::BumpAlloc) {
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
            reinterpret_cast<void*>(bump_slow_init),
            reinterpret_cast<void*>(g_real_malloc));
    } else { // ThreadLocalFreeList
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
    }

    if (n == 0) { munmap(page, CODE_PAGE_SIZE); return nullptr; }
    make_executable(page);
    return page;
}

void reclaim(void* code_page) {
    munmap(code_page, CODE_PAGE_SIZE);
}

} // namespace tbjit::codegen
