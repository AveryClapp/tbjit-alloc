#include "codegen/emitter.h"
#include "codegen/tls.h"
#include "codegen/slow_init.h"
#include <cassert>
#include <cstdint>
#include <sys/mman.h>

static bool g_deopt_called = false;

static void stub_deopt(uint32_t /*id*/, void* /*page*/) {
    g_deopt_called = true;
}

static void* stub_malloc(size_t sz) {
    return mmap(nullptr, sz, PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
}

static void test_emit_produces_bytes() {
    uint8_t buf[256] = {};
    uint32_t idx = tbjit::codegen::alloc_slot_index();
    [[maybe_unused]] size_t n = tbjit::codegen::emit_epoch_arena(
        buf, sizeof(buf),
        idx * 24, idx * 24 + 8,
        idx,
        48, 99,
        reinterpret_cast<void*>(stub_deopt),
        reinterpret_cast<void*>(tbjit::codegen::arena_reset_alloc),
        reinterpret_cast<void*>(stub_malloc));
    assert(n > 0 && n <= 256);
}

#if defined(__linux__) && defined(__x86_64__)
static void test_emitted_routine_executes_and_recycles() {
    void* page = mmap(nullptr, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);

    uint32_t idx = tbjit::codegen::alloc_slot_index();
    uintptr_t tp = reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
    uint32_t ptr_off = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&tbjit::codegen::tl_arenas[idx].ptr) - tp);
    uint32_t end_off = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&tbjit::codegen::tl_arenas[idx].end) - tp);

    size_t n = tbjit::codegen::emit_epoch_arena(
        static_cast<uint8_t*>(page), 4096,
        ptr_off, end_off,
        idx,
        48, 100,
        reinterpret_cast<void*>(stub_deopt),
        reinterpret_cast<void*>(tbjit::codegen::arena_reset_alloc),
        reinterpret_cast<void*>(stub_malloc));
    assert(n > 0);

    using AllocFn = void* (*)(size_t);
    AllocFn fn = reinterpret_cast<AllocFn>(page);

    // First call: arena uninitialized → slow path → mmaps region and returns base.
    void* p1 = fn(48);
    assert(p1 != nullptr);
    assert(!g_deopt_called);

    // Second call: fast path, ptr advances by 48.
    void* p2 = fn(48);
    assert(p2 == static_cast<uint8_t*>(p1) + 48);

    // Exhaust the region: arena_slow_init sized it for ARENA_REGION_SIZE,
    // so after ~5460 allocs we should wrap. Force a reset by manually
    // advancing tl_arenas[idx].ptr to the end.
    uint8_t* base = tbjit::codegen::tl_arenas[idx].base;
    uint8_t* end  = tbjit::codegen::tl_arenas[idx].end;
    tbjit::codegen::tl_arenas[idx].ptr = end;

    // Next call should take the reset path and return base.
    void* p3 = fn(48);
    assert(p3 == base);
    assert(!g_deopt_called);

    // Wrong size: deopts.
    g_deopt_called = false;
    fn(64);
    assert(g_deopt_called);

    munmap(page, 4096);
}
#endif

int main() {
    test_emit_produces_bytes();
#if defined(__linux__) && defined(__x86_64__)
    test_emitted_routine_executes_and_recycles();
#endif
    return 0;
}
