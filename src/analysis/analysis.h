#pragma once
#include "../common.h"

namespace tbjit::analysis {

void init();
void submit(const AllocEvent& ev);  // called from tracer flush
void run();                          // background thread entry point

} // namespace tbjit::analysis
