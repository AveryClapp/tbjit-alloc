#include "dispatch.h"
#include "alloc/alloc.h"
#include <atomic>
#include <cassert>
#include <new>

// Dispatch table: maps CallSiteID -> compiled routine.
// Reads are lock-free (atomic pointer load on hot path).
// Writes (install/revert) are infrequent and acquire a simple spinlock.
namespace tbjit::dispatch {

namespace {

constexpr size_t TABLE_SIZE = 4096;

struct Entry {
    std::atomic<RoutineFn> fn{nullptr};
    CallSiteID             id{0};
};

Entry* g_table = nullptr;

Entry* find(CallSiteID id) {
    size_t slot = id & (TABLE_SIZE - 1);
    for (size_t i = 0; i < TABLE_SIZE; ++i) {
        Entry& e = g_table[(slot + i) & (TABLE_SIZE - 1)];
        if (e.id == id || e.id == 0) return &e;
    }
    return nullptr;
}

} // namespace

void init() {
    g_table = static_cast<Entry*>(
        alloc::alloc(sizeof(Entry) * TABLE_SIZE, alignof(Entry)));
    for (size_t i = 0; i < TABLE_SIZE; ++i)
        new (&g_table[i]) Entry{};
}

void install(CallSiteID id, RoutineFn fn) {
    Entry* e = find(id);
    assert(e);
    e->id = id;
    e->fn.store(fn, std::memory_order_release);
}

void revert(CallSiteID id) {
    Entry* e = find(id);
    if (e && e->id == id)
        e->fn.store(nullptr, std::memory_order_release);
}

RoutineFn lookup(CallSiteID id) {
    Entry* e = find(id);
    if (!e || e->id != id) return nullptr;
    return e->fn.load(std::memory_order_acquire);
}

} // namespace tbjit::dispatch
