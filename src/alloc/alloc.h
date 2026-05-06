#pragma once
#include <cstddef>

// Internal mmap-backed bump allocator. Never calls malloc.
// All tbjit internals must use these instead of the system allocator.
namespace tbjit::alloc {

void  init();
void* alloc(size_t size, size_t align = alignof(max_align_t));

} // namespace tbjit::alloc
