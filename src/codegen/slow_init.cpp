#include "slow_init.h"
#include "tls.h"
#include <sys/mman.h>
#include <cassert>

namespace tbjit::codegen {

uint8_t* bump_slow_init(uint32_t index, uint32_t size) {
    void* mem = mmap(nullptr, BUMP_REGION_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);

    uint8_t* base = static_cast<uint8_t*>(mem);
    tl_bumps[index].ptr = base + size;
    tl_bumps[index].end = base + BUMP_REGION_SIZE;
    return base;
}

} // namespace tbjit::codegen
