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
    [[maybe_unused]] size_t n = tbjit::codegen::emit_freelist_alloc(
        buf, sizeof(buf),
        idx * 8,
        idx,
        48, 99,
        reinterpret_cast<void*>(stub_deopt),
        reinterpret_cast<void*>(tbjit::codegen::freelist_refill),
        reinterpret_cast<void*>(stub_malloc));
    assert(n > 0 && n <= 256);
}

#if defined(__linux__) && defined(__x86_64__)
static void test_emitted_routine_executes() {
    void* page = mmap(nullptr, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);

    uint32_t idx = tbjit::codegen::alloc_slot_index();
    uintptr_t tp = reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
    uint32_t head_off = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&tbjit::codegen::tl_freelists[idx].head) - tp);

    size_t n = tbjit::codegen::emit_freelist_alloc(
        static_cast<uint8_t*>(page), 4096,
        head_off,
        idx,
        48, 100,
        reinterpret_cast<void*>(stub_deopt),
        reinterpret_cast<void*>(tbjit::codegen::freelist_refill),
        reinterpret_cast<void*>(stub_malloc));
    assert(n > 0);

    using AllocFn = void* (*)(size_t);
    AllocFn fn = reinterpret_cast<AllocFn>(page);

    // First call: free list empty → refill → returns one chunk, list now has rest.
    void* p1 = fn(48);
    assert(p1 != nullptr);
    assert(!g_deopt_called);

    // Second call: pops the next chunk from the populated list.
    void* p2 = fn(48);
    assert(p2 != nullptr);
    assert(p2 != p1);
    assert(!g_deopt_called);

    // Manually push p1 back onto the list, then alloc should return it.
    *static_cast<void**>(p1) = tbjit::codegen::tl_freelists[idx].head;
    tbjit::codegen::tl_freelists[idx].head = p1;
    void* p3 = fn(48);
    assert(p3 == p1);

    // Wrong size: hits deopt stub.
    g_deopt_called = false;
    fn(64);
    assert(g_deopt_called);

    munmap(page, 4096);
}
#endif // __linux__ && __x86_64__

int main() {
    test_emit_produces_bytes();
#if defined(__linux__) && defined(__x86_64__)
    test_emitted_routine_executes();
#endif
    return 0;
}
