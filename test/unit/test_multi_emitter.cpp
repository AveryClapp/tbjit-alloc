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

// Emitter writes a non-empty byte sequence; first instruction is cmp rdi, imm32
// with the first class size as the immediate.
static void test_emitter_writes_expected_prologue() {
    uint8_t buf[1024]{};
    const uint32_t class_sizes[] = {32, 96};
    size_t n = emit_multi_freelist_alloc(
        buf, sizeof(buf),
        /*heads_off=*/0x10,
        /*slot=*/7,
        class_sizes, /*count=*/2,
        /*site=*/42,
        /*deopt=*/reinterpret_cast<void*>(0xDEAD0000),
        /*refill=*/reinterpret_cast<void*>(0xBEEF0000),
        /*real_malloc=*/reinterpret_cast<void*>(0xCAFE0000));
    CHECK(n > 0);
    CHECK(n <= sizeof(buf));

    // cmp rdi, imm32 = 48 81 FF <imm32>
    CHECK(buf[0] == 0x48);
    CHECK(buf[1] == 0x81);
    CHECK(buf[2] == 0xFF);
    uint32_t imm0; std::memcpy(&imm0, &buf[3], 4);
    CHECK(imm0 == 32);

    // After the first class body, a second cmp rdi, imm32 with class[1] size.
    // Find it by scanning.
    bool found_second_cmp = false;
    for (size_t i = 10; i + 7 < n; ++i) {
        if (buf[i] == 0x48 && buf[i + 1] == 0x81 && buf[i + 2] == 0xFF) {
            uint32_t imm; std::memcpy(&imm, &buf[i + 3], 4);
            if (imm == 96) { found_second_cmp = true; break; }
        }
    }
    CHECK(found_second_cmp);
}

// Reject K=0 and K>4.
static void test_emitter_rejects_invalid_class_count() {
    uint8_t buf[1024]{};
    const uint32_t five[] = {16, 32, 48, 64, 80};
    CHECK(emit_multi_freelist_alloc(buf, sizeof(buf), 0, 0, five, 5, 0,
        nullptr, nullptr, nullptr) == 0);
    CHECK(emit_multi_freelist_alloc(buf, sizeof(buf), 0, 0, five, 0, 0,
        nullptr, nullptr, nullptr) == 0);
}

// Reject buffer too small.
static void test_emitter_rejects_small_buffer() {
    uint8_t buf[64]{};
    const uint32_t one[] = {32};
    CHECK(emit_multi_freelist_alloc(buf, sizeof(buf), 0, 0, one, 1, 0,
        nullptr, nullptr, nullptr) == 0);
}

// multi_refill carves a fresh segment for the requested class and
// installs heads[class_idx]. Returns one chunk.
static void test_multi_refill_carves_segment() {
    g_slot_to_site[0] = 99;
    tl_multi_freelists[0] = {};   // zero state
    void* chunk = multi_refill(/*slot=*/0, /*size=*/48, /*class_idx=*/2);
    CHECK(chunk);
    seg::SegmentHeader* s = seg::of(chunk);
    CHECK(seg::is_managed(s));
    CHECK(s->strategy == Strategy::MultiSizeFreeList);
    CHECK(s->class_idx == 2);
    CHECK(s->chunk_size == 48);
    CHECK(tl_multi_freelists[0].segs[2] == s);
    CHECK(tl_multi_freelists[0].heads[2] != nullptr);
    seg::free_segment(s);
}

int main() {
    test_emitter_writes_expected_prologue();
    test_emitter_rejects_invalid_class_count();
    test_emitter_rejects_small_buffer();
    test_multi_refill_carves_segment();
    std::puts("test_multi_emitter OK");
    return 0;
}
