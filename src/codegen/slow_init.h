#pragma once
#include <cstddef>
#include <cstdint>

namespace tbjit::codegen {

constexpr size_t BUMP_REGION_SIZE = 256 * 1024;

// Called from emitted slow path. mmaps a fresh region, installs it into
// tl_bumps[index], and returns the first allocation pointer.
// size is the requested allocation size (already guard-checked to be correct).
uint8_t* bump_slow_init(uint32_t index, uint32_t size);

} // namespace tbjit::codegen
