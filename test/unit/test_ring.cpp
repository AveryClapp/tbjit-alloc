#include "trace/trace.h"
#include "alloc/alloc.h"
#include <cassert>

int main() {
    tbjit::alloc::init();
    tbjit::trace::init();

    // Record a batch of alloc events and verify no crash.
    for (int i = 0; i < 100; ++i) {
        void* fake_ptr = reinterpret_cast<void*>(0x1000 + i);
        tbjit::trace::record_alloc(static_cast<tbjit::CallSiteID>(i), 64, fake_ptr);
        tbjit::trace::record_free(static_cast<tbjit::CallSiteID>(i), fake_ptr);
    }

    return 0;
}
