#include "dispatch/dispatch.h"
#include "alloc/alloc.h"
#include <cassert>

static void* dummy_routine(size_t) { return nullptr; }

int main() {
    tbjit::alloc::init();
    tbjit::dispatch::init();

    tbjit::CallSiteID id = 0xdeadbeef;

    assert(tbjit::dispatch::lookup(id) == nullptr);

    tbjit::dispatch::install(id, dummy_routine);
    assert(tbjit::dispatch::lookup(id) == dummy_routine);

    tbjit::dispatch::revert(id);
    assert(tbjit::dispatch::lookup(id) == nullptr);

    return 0;
}
