/* Gate 1 ground-truth probe: isolate transparent-hugepage RSS slack with a
 * KNOWN lifetime mix, so we can measure slack to the byte instead of inferring
 * it. The whole option-2 thesis rests on "churny allocations carry hugepage
 * slack that a lifetime-aware policy could shed without losing the TLB win on
 * long-lived data." This program manufactures exactly that mix:
 *
 *   - a long-lived HELD set (allocated once, kept resident, never freed) ->
 *     the data a good policy SHOULD keep on hugepages.
 *   - a CHURN stream with a fixed live window but unbounded turnover ->
 *     the data whose freed-but-hugepaged regions become RSS slack under THP.
 *
 * Because we know live bytes exactly, slack = Rss - live is a clean number.
 * Run the same binary under glibc / mimalloc / jemalloc{never,always,default}
 * via LD_PRELOAD and compare AnonHugePages and slack across configs. If
 * jemalloc's heuristic already drives AnonHugePages ~ live (sheds the churn
 * slack) the niche is gone; if always-mode bloats it and the heuristic does
 * not recover it, there is room for a lifetime-aware policy.
 *
 * Self-reports from /proc/self/smaps_rollup at steady state (after churn,
 * before exit) to avoid racing the harness against process teardown. Prints a
 * single TSV row to stdout; diagnostics to stderr. Linux-only at runtime
 * (compiles anywhere; non-Linux prints NA for the kernel-sourced fields).
 *
 * Usage: gate1_lifetime_probe [held_count held_kb churn_window churn_kb iters]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Read one "Key:   N kB" field from smaps_rollup; returns kB or -1. */
static long read_smaps_kb(const char *key) {
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (!f) return -1;
    char line[256];
    size_t klen = strlen(key);
    long val = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0) {
            sscanf(line + klen, " %ld", &val);
            break;
        }
    }
    fclose(f);
    return val;
}

/* Touch every page so the mapping is actually resident (THP only matters for
 * faulted pages). volatile sink prevents the writes being optimized away. */
static volatile uint8_t g_sink;
static void touch(uint8_t *p, size_t bytes) {
    for (size_t i = 0; i < bytes; i += 4096) p[i] = (uint8_t)(i ^ 0x5a);
    g_sink ^= p[0];
}

int main(int argc, char **argv) {
    /* Defaults: 4 MiB held (64 x 64 KiB), 1 MiB live churn window (256 x 4 KiB),
     * ~8 GiB of churn turnover. The held set is large/long-lived (THP-worthy);
     * the churn window is small but its arena spans many regions over time. */
    size_t held_count   = (argc > 1) ? strtoul(argv[1], 0, 10) : 64;
    size_t held_kb      = (argc > 2) ? strtoul(argv[2], 0, 10) : 64;
    size_t churn_window = (argc > 3) ? strtoul(argv[3], 0, 10) : 256;
    size_t churn_kb     = (argc > 4) ? strtoul(argv[4], 0, 10) : 4;
    size_t iters        = (argc > 5) ? strtoul(argv[5], 0, 10) : 2000000;

    size_t held_bytes  = held_kb  * 1024;
    size_t churn_bytes = churn_kb * 1024;

    /* Long-lived held set: allocate once, touch, keep referenced forever. */
    uint8_t **held = (uint8_t **)calloc(held_count, sizeof *held);
    if (!held) { perror("calloc held"); return 1; }
    for (size_t i = 0; i < held_count; i++) {
        held[i] = (uint8_t *)malloc(held_bytes);
        if (!held[i]) { perror("malloc held"); return 1; }
        touch(held[i], held_bytes);
    }

    /* Churn: fixed live window, unbounded turnover. Live churn bytes stay at
     * churn_window * churn_bytes; total allocated grows with iters, so the
     * allocator's arena sprawls across regions that THP may keep resident. */
    uint8_t **win = (uint8_t **)calloc(churn_window, sizeof *win);
    if (!win) { perror("calloc win"); return 1; }
    for (size_t i = 0; i < iters; i++) {
        size_t s = i % churn_window;
        if (win[s]) free(win[s]);
        win[s] = (uint8_t *)malloc(churn_bytes);
        if (win[s]) touch(win[s], churn_bytes);
    }

    /* Measure at steady state. live = held + current churn window. */
    long live_kb = (long)(held_count * held_kb + churn_window * churn_kb);
    long rss     = read_smaps_kb("Rss:");
    long anon    = read_smaps_kb("Anonymous:");
    long thp     = read_smaps_kb("AnonHugePages:");
    long slack   = (rss >= 0) ? rss - live_kb : -1;

    /* Single TSV row: live, then kernel-sourced resident/anon/thp, then slack.
     * -1 reads print as NA so the harness can tell "not on Linux" from a real
     * zero. */
    printf("%ld\t%ld\t%ld\t%ld\t%ld\n", live_kb, rss, anon, thp, slack);
    fprintf(stderr, "[probe] held=%zux%zuKiB churn_win=%zux%zuKiB iters=%zu "
            "live=%ldkB rss=%ldkB thp=%ldkB slack=%ldkB\n",
            held_count, held_kb, churn_window, churn_kb, iters,
            live_kb, rss, thp, slack);

    /* Keep held referenced past measurement so it cannot be reclaimed early. */
    g_sink ^= held[0][0];
    return 0;
}
