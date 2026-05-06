#include "alloc.h"
#include <sys/mman.h>
#include <cassert>
#include <cstdint>

namespace tbjit::alloc {

namespace {

constexpr size_t ARENA_SIZE = 4 * 1024 * 1024; // 4 MiB

struct Arena {
    uint8_t* base;
    uint8_t* cursor;
    uint8_t* end;
};

Arena g_arena{};

} // namespace

void init() {
    void* mem = mmap(nullptr, ARENA_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);
    g_arena.base   = static_cast<uint8_t*>(mem);
    g_arena.cursor = g_arena.base;
    g_arena.end    = g_arena.base + ARENA_SIZE;
}

void* alloc(size_t size, size_t align) {
    uintptr_t cur  = reinterpret_cast<uintptr_t>(g_arena.cursor);
    uintptr_t aligned = (cur + align - 1) & ~(align - 1);
    uint8_t*  next = reinterpret_cast<uint8_t*>(aligned) + size;
    assert(next <= g_arena.end && "internal arena exhausted");
    g_arena.cursor = next;
    return reinterpret_cast<void*>(aligned);
}

} // namespace tbjit::alloc
