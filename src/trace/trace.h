#pragma once
#include "../common.h"
#include <atomic>
#include <cstddef>

namespace tbjit::trace {

struct RingBuffer {
    static constexpr size_t CAPACITY = 1024;

    AllocEvent slots[CAPACITY];
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
    std::atomic<RingBuffer*> next{nullptr};

    // Returns true if buffer transitioned empty->non-empty (caller should wake background thread).
    bool push(const AllocEvent& ev);
    bool pop(AllocEvent& out);
};

void        init();
void        record_alloc(CallSiteID id, size_t size, void* ptr);
void        record_free(CallSiteID id, void* ptr);
RingBuffer* ring_head();  // head of global linked list, for background thread

} // namespace tbjit::trace
