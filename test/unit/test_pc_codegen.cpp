#include "codegen/emitter.h"
#include "codegen/slow_init.h"
#include "codegen/tls.h"
#include "seg/segment.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "fail: %s @ %s:%d\n", #c, __FILE__, __LINE__); std::abort(); } } while (0)

using namespace tbjit;
using namespace tbjit::codegen;

// Emitter writes non-empty output and the prologue is `cmp rdi, size`.
static void test_pc_emitter_prologue() {
    uint8_t buf[1024]{};
    size_t n = emit_pc_alloc(
        buf, sizeof(buf), 0x10, 0x18,
        /*slot=*/3, /*size=*/64, /*site=*/77,
        reinterpret_cast<void*>(0xDEAD0000),
        reinterpret_cast<void*>(0xBEEF0000),
        reinterpret_cast<void*>(0xCAFE0000));
    CHECK(n > 0);
    CHECK(buf[0] == 0x48);
    CHECK(buf[1] == 0x81);
    CHECK(buf[2] == 0xFF);
    uint32_t sz; std::memcpy(&sz, &buf[3], 4);
    CHECK(sz == 64);
}

// pc_refill creates a ProducerConsumer segment, installs into tl_bumps, and
// returns the first chunk.
static void test_pc_refill_initial() {
    uint32_t slot = alloc_slot_index();
    g_slot_to_site[slot] = 4242;
    tl_bumps[slot] = {};   // ensure clean state
    uint8_t* p = pc_refill(slot, 48);
    CHECK(p);
    seg::SegmentHeader* s = seg::of(p);
    CHECK(seg::is_managed(s));
    CHECK(s->strategy == Strategy::ProducerConsumer);
    CHECK(s->chunk_size == 48);
    CHECK(tl_bumps[slot].ptr == p + 48);
    CHECK(tl_bumps[slot].end == seg::segment_end(s));
}

// Second call to pc_refill retires the prior segment with live_chunks set
// to the served count minus MPSC-drained.
static void test_pc_refill_retires_prior() {
    uint32_t slot = alloc_slot_index();
    g_slot_to_site[slot] = 8888;
    tl_bumps[slot] = {};
    uint8_t* p1 = pc_refill(slot, 64);
    seg::SegmentHeader* old = seg::of(p1);

    // Simulate "served 5 chunks total" by advancing bump_ptr.
    old->bump_ptr = seg::payload_start(old) + 5 * 64;
    tl_bumps[slot].ptr = old->bump_ptr;

    // Simulate 2 chunks already freed back to consumer via MPSC.
    uint8_t* base = seg::payload_start(old);
    seg::mpsc_push(old, base + 1 * 64);
    seg::mpsc_push(old, base + 2 * 64);

    // Trigger second refill; old should retire with live_chunks = 5 - 2 = 3.
    uint8_t* p2 = pc_refill(slot, 64);
    CHECK(p2 != p1);
    CHECK(old->retired == true);
    CHECK(old->live_chunks.load() == 3);

    seg::SegmentHeader* fresh = seg::of(p2);
    CHECK(fresh->strategy == Strategy::ProducerConsumer);
    CHECK(fresh->retired == false);
}

int main() {
    test_pc_emitter_prologue();
    test_pc_refill_initial();
    test_pc_refill_retires_prior();
    std::puts("test_pc_codegen OK");
    return 0;
}
