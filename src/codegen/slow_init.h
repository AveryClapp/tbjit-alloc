#pragma once
#include <cstddef>
#include <cstdint>

namespace tbjit::codegen {

constexpr size_t BUMP_REGION_SIZE = 256 * 1024;

// Called from emitted slow path. mmaps a fresh region, installs it into
// tl_bumps[index], and returns the first allocation pointer.
// size is the requested allocation size (already guard-checked to be correct).
uint8_t* bump_slow_init(uint32_t index, uint32_t size);

// True if ptr lies inside any registered bump region. Used by the free
// interceptor to skip glibc free() for JIT-served pointers (those pointers
// were carved out of an mmap'd region and would crash glibc's allocator).
bool is_in_bump_region(const void* ptr);

} // namespace tbjit::codegen
