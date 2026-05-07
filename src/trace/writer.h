#pragma once
#include "../common.h"
#include <cstdint>

namespace tbjit::trace {

constexpr uint32_t TRACE_MAGIC   = 0x54424A54; // "TBJT"
constexpr uint32_t TRACE_VERSION = 1;

struct TraceHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t start_ns;  // process start timestamp
};

void writer_open(const char* path);
void writer_write(const AllocEvent& ev);
void writer_close();
bool writer_active();

} // namespace tbjit::trace
