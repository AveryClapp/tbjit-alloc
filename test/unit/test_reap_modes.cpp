// Pins the eager reap policy: with TBJIT_REAP_MODE=eager the Reap-tag gate is
// dropped, so a retired+empty segment is reclaimed even when the predicate says
// "never reap" — relying solely on the live_chunks==0 safety invariant. Env is
// set before any reaper_sweep so seg::reap_mode()'s lazy static caches "eager".
#include <cstdlib>
static const int set_mode = (setenv("TBJIT_REAP_MODE", "eager", 1), 0);

#include "seg/segment.h"
#include <cstdio>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit;
using namespace tbjit::seg;

static bool never_reap(CallSiteID) { return false; }

int main() {
    (void) set_mode;
    CHECK(reap_mode() == ReapMode::Eager);

    SegmentHeader* s = alloc_segment(Strategy::ThreadLocalFreeList, 0, 11, 48);
    CHECK(s);
    s->retired = true;
    s->live_chunks.store(0);

    // Conservative would skip this (never_reap), eager reclaims regardless.
    size_t n = reaper_sweep(never_reap);
    CHECK(n >= 1);
    CHECK(!is_managed(s));

    // live_chunks != 0 must still block reclaim under eager.
    SegmentHeader* s2 = alloc_segment(Strategy::ThreadLocalFreeList, 1, 22, 48);
    CHECK(s2);
    s2->retired = true;
    s2->live_chunks.store(2);
    CHECK(reaper_sweep(never_reap) == 0);
    CHECK(is_managed(s2));
    free_segment(s2);

    std::puts("test_reap_modes OK");
    return 0;
}
