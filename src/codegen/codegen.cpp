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
    if (spec.strategy != Strategy::BumpAlloc) return nullptr;
    if (spec.size == 0 || spec.size >= tbjit::analysis::ExactHistogram::MAX_SIZE) return nullptr;

    uint32_t idx = alloc_slot_index();
    if (idx >= static_cast<uint32_t>(MAX_COMPILED_SITES)) return nullptr;

    uint32_t ptr_off = idx * static_cast<uint32_t>(sizeof(BumpSlot));
    uint32_t end_off = ptr_off + 8;

    uint8_t* page = static_cast<uint8_t*>(alloc_exec_page());
    size_t n = emit_bump_alloc(
        page, CODE_PAGE_SIZE,
        ptr_off, end_off,
        spec.size, spec.id,
        reinterpret_cast<void*>(tbjit::deopt::handle),
        reinterpret_cast<void*>(bump_slow_init),
        reinterpret_cast<void*>(g_real_malloc));

    if (n == 0) { munmap(page, CODE_PAGE_SIZE); return nullptr; }
    make_executable(page);
    return page;
}

void reclaim(void* code_page) {
    munmap(code_page, CODE_PAGE_SIZE);
}

} // namespace tbjit::codegen
