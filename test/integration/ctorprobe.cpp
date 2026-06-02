/* Probe library: a DT_NEEDED dependency whose constructor allocates during
 * _dl_init. glibc initializes DT_NEEDED dependencies before an LD_PRELOAD'd
 * object, so this constructor runs before tbjit's constructor has resolved the
 * real allocator via dlsym -- the exact ordering that crashed libselinux
 * (find_so SIGSEGV) and libLLVM (clang++ SIGABRT). A pre-fix tbjit returns NULL
 * here; the bootstrap-arena fix returns usable memory.
 *
 * Uses only raw write()/_exit() (no stdio) so the probe itself never recurses
 * back into the allocator while we are testing the allocator.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor))
static void tbjit_ctor_probe(void) {
    void* p = malloc(4096);
    if (!p) { (void)write(2, "PROBE_NULL\n", 11); _exit(33); }
    memset(p, 0xAB, 4096);   /* the returned region must be writable */
    free(p);                 /* a bootstrap chunk frees as a no-op */
    (void)write(1, "PROBE_CTOR_OK\n", 14);
}
