#include "shadow.h"
#ifdef TBJIT_SHADOW

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "seg/segment.h"

static inline unsigned shadow_tid() {
    return static_cast<unsigned>(syscall(SYS_gettid));
}

// Shadow validator records every JIT-served allocation in an open-addressing
// hash table keyed by pointer. On free we look the pointer up and erase the
// entry. Untracked pointers (those from the generic glibc path) are silently
// ignored. Violations of the invariants (null return, misaligned pointer,
// double-alloc on a live slot) abort so faults in the JIT path surface
// immediately rather than as delayed heap corruption.
//
// Compiles only into libtbjit_shadow (the parallel binary used for debug/CI
// runs); production libtbjit gets the no-op inline stubs from shadow.h.

namespace tbjit::shadow {

namespace {

struct Entry {
    void*       ptr;       // null = empty slot
    size_t      size;
    CallSiteID  alloc_id;
    unsigned    alloc_tid; // tid that produced this live allocation
};

constexpr size_t TABLE_SIZE = 1u << 17;  // 128k slots — covers ~5460 live * many call sites
Entry            g_table[TABLE_SIZE];
pthread_mutex_t  g_mutex = PTHREAD_MUTEX_INITIALIZER;

size_t hash_ptr(void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    v ^= v >> 21;
    v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27;
    return static_cast<size_t>(v) & (TABLE_SIZE - 1);
}

[[noreturn]] void fail(const char* msg, void* ptr) {
    fprintf(stderr, "tbjit shadow: %s ptr=%p\n", msg, ptr);
    std::abort();
}

} // namespace

void validate_alloc(CallSiteID id, size_t size, void* jit_ptr) {
    if (!jit_ptr) fail("alloc returned null", jit_ptr);
    if ((reinterpret_cast<uintptr_t>(jit_ptr) % alignof(max_align_t)) != 0)
        fail("alloc misaligned", jit_ptr);

    unsigned tid = shadow_tid();
    pthread_mutex_lock(&g_mutex);
    size_t i = hash_ptr(jit_ptr);
    size_t scanned = 0;
    while (g_table[i].ptr != nullptr) {
        if (g_table[i].ptr == jit_ptr) {
            Entry e = g_table[i];
            pthread_mutex_unlock(&g_mutex);
            bool mgd = tbjit::seg::is_managed(tbjit::seg::of(jit_ptr));
            fprintf(stderr,
                "tbjit shadow: DUP ptr=%p managed=%d  existing{size=%zu id=%u tid=%u}  "
                "new{size=%zu id=%u tid=%u}  same_thread=%d same_site=%d\n",
                jit_ptr, mgd, e.size, e.alloc_id, e.alloc_tid,
                size, id, tid,
                (e.alloc_tid == tid), (e.alloc_id == id));
            std::abort();
        }
        i = (i + 1) & (TABLE_SIZE - 1);
        if (++scanned >= TABLE_SIZE) {
            pthread_mutex_unlock(&g_mutex);
            fail("shadow table full", jit_ptr);
        }
    }
    g_table[i] = {jit_ptr, size, id, tid};
    pthread_mutex_unlock(&g_mutex);
}

void validate_free(CallSiteID /*free_id*/, void* ptr) {
    if (!ptr) return;  // free(NULL) is a well-defined no-op
    pthread_mutex_lock(&g_mutex);
    size_t i = hash_ptr(ptr);
    size_t scanned = 0;
    while (g_table[i].ptr != ptr) {
        if (g_table[i].ptr == nullptr) {
            // Untracked: came from the generic path, not the JIT.
            pthread_mutex_unlock(&g_mutex);
            return;
        }
        i = (i + 1) & (TABLE_SIZE - 1);
        if (++scanned >= TABLE_SIZE) {
            pthread_mutex_unlock(&g_mutex);
            return;  // not present
        }
    }
    // Backward shift to keep the cluster densely packed (linear-probing
    // deletion). Without this an unlucky cluster could leak slots.
    g_table[i].ptr = nullptr;
    size_t j = (i + 1) & (TABLE_SIZE - 1);
    while (g_table[j].ptr != nullptr) {
        Entry e = g_table[j];
        g_table[j].ptr = nullptr;
        size_t k = hash_ptr(e.ptr);
        while (g_table[k].ptr != nullptr) k = (k + 1) & (TABLE_SIZE - 1);
        g_table[k] = e;
        j = (j + 1) & (TABLE_SIZE - 1);
    }
    pthread_mutex_unlock(&g_mutex);
}

} // namespace tbjit::shadow
#endif // TBJIT_SHADOW
