#include "trace.h"
#include "dispatch/dispatch.h"
#include "deopt/deopt.h"
#include "shadow/shadow.h"
#include "common.h"
#include <cstdlib>
#include <dlfcn.h>
#include <pthread.h>

// LD_PRELOAD entry points. These intercept every malloc/free call
// in the target process and route them through the dispatch table.
// reentrancy_guard prevents recursive interposition when tbjit
// internals legitimately call the real allocator (they shouldn't,
// but this is a safety net during development).

namespace {

thread_local bool reentrancy_guard = false;

using malloc_fn = void* (*)(size_t);
using free_fn   = void  (*)(void*);

malloc_fn real_malloc = nullptr;
free_fn   real_free   = nullptr;

__attribute__((constructor))
void tbjit_init() {
    real_malloc = reinterpret_cast<malloc_fn>(dlsym(RTLD_NEXT, "malloc"));
    real_free   = reinterpret_cast<free_fn>(dlsym(RTLD_NEXT, "free"));
    tbjit::trace::init();
    tbjit::dispatch::init();
    tbjit::deopt::init();
}

} // namespace

extern "C" {

void* malloc(size_t size) {
    if (reentrancy_guard || !real_malloc) return real_malloc(size);
    reentrancy_guard = true;
    tbjit::deopt::mark_safe_point();

    void* ra = __builtin_return_address(0);
    tbjit::CallSiteID id = tbjit::hash_return_addr(ra);

    tbjit::dispatch::RoutineFn fn = tbjit::dispatch::lookup(id);
    void* ptr;
    if (__builtin_expect(fn != nullptr, 1)) {
        ptr = fn(size);
    } else {
        ptr = real_malloc(size);
        tbjit::trace::record_alloc(id, size, ptr);
    }

    tbjit::shadow::validate_alloc(id, size, ptr);
    reentrancy_guard = false;
    return ptr;
}

void free(void* ptr) {
    if (reentrancy_guard || !real_free) { real_free(ptr); return; }
    reentrancy_guard = true;

    void* ra = __builtin_return_address(0);
    tbjit::CallSiteID id = tbjit::hash_return_addr(ra);
    tbjit::trace::record_free(id, ptr);
    tbjit::shadow::validate_free(id, ptr);
    real_free(ptr);

    reentrancy_guard = false;
}

} // extern "C"
