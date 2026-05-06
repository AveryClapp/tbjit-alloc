#pragma once
#include "../common.h"
#include <cstddef>

namespace tbjit::dispatch {

using RoutineFn = void* (*)(size_t size);

void      init();
void      install(CallSiteID id, RoutineFn fn);
void      revert(CallSiteID id);         // back to generic path
RoutineFn lookup(CallSiteID id);

} // namespace tbjit::dispatch
