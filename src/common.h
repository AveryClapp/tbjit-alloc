#pragma once
#include <cstddef>
#include <cstdint>

namespace tbjit {

using CallSiteID = uint32_t;

// Hashes a return address into a call-site identifier.
inline CallSiteID hash_return_addr(void* ra) {
    uintptr_t v = reinterpret_cast<uintptr_t>(ra);
    v ^= v >> 16;
    v *= 0x45d9f3b;
    v ^= v >> 16;
    return static_cast<CallSiteID>(v);
}

enum class Strategy : uint8_t {
    Generic             = 0,
    BumpAlloc           = 1,
    ThreadLocalFreeList = 2,
    EpochArena          = 3,
    PairedStack         = 4,
};

struct AllocEvent {
    CallSiteID  call_site;
    uint32_t    size;
    uint64_t    timestamp_ns;
    uint32_t    thread_id;
    void*       ptr;        // filled on free; null on alloc
};

} // namespace tbjit
