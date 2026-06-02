#pragma once
#include "../common.h"
#include "analysis/analysis.h"

namespace tbjit::deopt {

void init();

// Called by a compiled routine when its guard fails. The emitted deopt
// epilogue passes a ground-truth reason (3rd arg, edx) so the picker records
// *why* the site deopted instead of inferring it from the strategy.
// Reverts dispatch entry to generic path and restarts analysis.
void handle(CallSiteID id, void* code_page,
            analysis::DeoptReason reason = analysis::DeoptReason::Other);

// Epoch safe-point: a thread calls this on re-entry to the allocator.
// Once all threads have checked in past epoch N, pages queued for
// reclamation at epoch N can be freed.
void mark_safe_point();

// Background-thread-only: drains the deopt queue (running reset_call_site
// for each entry), then reclaims pages whose epoch has been observed by
// every registered thread. Must NOT be called from allocator threads.
void drain_pending();

} // namespace tbjit::deopt
