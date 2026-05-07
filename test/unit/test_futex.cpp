#include "analysis/futex.h"
#include <cassert>
#include <pthread.h>
#include <atomic>

static std::atomic<int> g_woken{0};

static void* waiter(void*) {
    tbjit::analysis::futex_wait();
    g_woken.store(1, std::memory_order_release);
    return nullptr;
}

static void test_wake() {
    pthread_t t;
    pthread_create(&t, nullptr, waiter, nullptr);
    struct timespec ts{0, 10'000'000}; // 10ms — give thread time to park
    nanosleep(&ts, nullptr);
    tbjit::analysis::futex_wake();
    pthread_join(t, nullptr);
    assert(g_woken.load() == 1);
}

int main() {
    test_wake();
    return 0;
}
