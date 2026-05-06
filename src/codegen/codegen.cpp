#include "codegen.h"
#include "alloc/alloc.h"
#include <sys/mman.h>
#include <cstring>
#include <cassert>

// x86-64 code emitter. Each compiled routine owns one mmap'd executable page.
// Guards at the top of every routine; failure branches to the deopt handler.
namespace tbjit::codegen {

namespace {

constexpr size_t CODE_PAGE_SIZE = 4096;

void* alloc_exec_page() {
    void* p = mmap(nullptr, CODE_PAGE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    return p;
}

void make_executable(void* page) {
    mprotect(page, CODE_PAGE_SIZE, PROT_READ | PROT_EXEC);
}

} // namespace

void* compile(const RoutineSpec& spec) {
    uint8_t* page = static_cast<uint8_t*>(alloc_exec_page());
    // TODO: emit x86-64 based on spec.strategy
    // Each strategy: emit guards, fast-path body, deopt branch
    (void)spec;
    make_executable(page);
    return page;
}

void reclaim(void* code_page) {
    munmap(code_page, CODE_PAGE_SIZE);
}

} // namespace tbjit::codegen
