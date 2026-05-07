#include "trace/trace.h"
#include "alloc/alloc.h"
#include <cassert>
#include <pthread.h>

static void test_registry_non_null() {
    tbjit::alloc::init();
    tbjit::trace::init();
    void* fake = reinterpret_cast<void*>(0x1000);
    tbjit::trace::record_alloc(1, 48, fake);
    assert(tbjit::trace::ring_head() != nullptr);
}

static void* thread_push(void*) {
    void* fake = reinterpret_cast<void*>(0x2000);
    tbjit::trace::record_alloc(2, 64, fake);
    return nullptr;
}

static void test_registry_multithread() {
    pthread_t t;
    pthread_create(&t, nullptr, thread_push, nullptr);
    pthread_join(t, nullptr);

    int count = 0;
    tbjit::trace::RingBuffer* n = tbjit::trace::ring_head();
    while (n) { ++count; n = n->next.load(std::memory_order_relaxed); }
    assert(count >= 2);
}

int main() {
    test_registry_non_null();
    test_registry_multithread();
    return 0;
}
