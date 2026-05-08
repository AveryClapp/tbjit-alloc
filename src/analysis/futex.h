#pragma once
#include <atomic>
#include <cstdint>

#ifdef __linux__
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

namespace tbjit::analysis {

enum FutexState : uint32_t { AWAKE = 0, SLEEPING = 1 };

inline std::atomic<uint32_t>& futex_word() {
    static std::atomic<uint32_t> word{AWAKE};
    return word;
}

inline void futex_wait() {
#ifdef __linux__
    uint32_t expected = AWAKE;
    futex_word().compare_exchange_strong(expected, SLEEPING,
        std::memory_order_acq_rel, std::memory_order_relaxed);
    uint32_t* addr = reinterpret_cast<uint32_t*>(&futex_word());
    struct timespec timeout{1, 0};  // 1s timeout: bounds missed-wakeup hang
    syscall(SYS_futex, addr, FUTEX_WAIT, SLEEPING, &timeout, nullptr, 0);
    futex_word().store(AWAKE, std::memory_order_release);
#endif
}

inline void futex_wake() {
#ifdef __linux__
    futex_word().store(AWAKE, std::memory_order_release);
    uint32_t* addr = reinterpret_cast<uint32_t*>(&futex_word());
    syscall(SYS_futex, addr, FUTEX_WAKE, 1, nullptr, nullptr, 0);
#endif
}

// Call after a ring buffer push that returned true (empty->non-empty transition).
inline void notify_if_sleeping() {
    if (futex_word().load(std::memory_order_acquire) == SLEEPING)
        futex_wake();
}

} // namespace tbjit::analysis
