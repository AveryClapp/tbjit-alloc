#pragma once
#include "../common.h"
#include <cstddef>

namespace tbjit::trace {

void init();
void record_alloc(CallSiteID id, size_t size, void* ptr);
void record_free(CallSiteID id, void* ptr);

} // namespace tbjit::trace
