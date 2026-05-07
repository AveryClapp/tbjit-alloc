#include "trace/writer.h"
#include "alloc/alloc.h"
#include <cassert>
#include <cstdio>

static void test_write_and_read_back() {
    tbjit::alloc::init();
    const char* path = "/tmp/tbjit_test_trace.bin";
    tbjit::trace::writer_open(path);
    assert(tbjit::trace::writer_active());

    tbjit::AllocEvent ev{42, 48, 12345, 1, reinterpret_cast<void*>(0xdeadbeef)};
    tbjit::trace::writer_write(ev);
    tbjit::trace::writer_close();
    assert(!tbjit::trace::writer_active());

    FILE* f = fopen(path, "rb");
    assert(f != nullptr);

    tbjit::trace::TraceHeader hdr;
    assert(fread(&hdr, sizeof(hdr), 1, f) == 1);
    assert(hdr.magic == tbjit::trace::TRACE_MAGIC);
    assert(hdr.version == tbjit::trace::TRACE_VERSION);

    tbjit::AllocEvent read_ev;
    assert(fread(&read_ev, sizeof(read_ev), 1, f) == 1);
    assert(read_ev.call_site == 42);
    assert(read_ev.size == 48);
    assert(read_ev.ptr == reinterpret_cast<void*>(0xdeadbeef));

    fclose(f);
    remove(path);
}

static void test_writer_inactive_by_default() {
    assert(!tbjit::trace::writer_active());
}

int main() {
    test_writer_inactive_by_default();
    test_write_and_read_back();
    return 0;
}
