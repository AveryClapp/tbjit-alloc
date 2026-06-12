#!/usr/bin/env python3
"""Cost-of-analysis overhead report for the tbjit allocator.

Phase 3a.6: honest accounting of what the JIT picker costs. Reviewers will
ask "what's the overhead?" and "isn't the g_summaries array huge?". This
pulls the answers out of one real-workload run:

  - Memory: resident RSS delta (tbjit vs glibc) per workload, from
    manifest.tsv max_rss_kb. Plus the structural costs: compiled code pages
    (compiled-count x 4 KiB), live segment headers (64 B each), and the
    g_summaries array (mmap'd lazily, so its virtual size is *not*
    resident — only touched call-site slots are).
  - Time: wall delta (tbjit vs glibc) per workload, and the per-call
    trampoline cost from bench/bench_micro_jit_direct.cpp (pass --ns-per-call
    to override the default; run the microbench to refresh it).

Usage:
  cost_report.py <realworkload-dir> [--ns-per-call N] [--format md|text]

<realworkload-dir> has manifest.tsv and a json/ dir of TBJIT_DUMP_JSON dumps
(the layout run_suite.sh produces).
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_dumps import load_dumps  # noqa: E402

# Structural constants, mirrored from the source (single source of truth is the
# code; these are for the report only).
CODE_PAGE_BYTES = 4096           # src/codegen/codegen.cpp CODE_PAGE_SIZE
SEGMENT_HEADER_BYTES = 64        # src/seg/segment.h SegmentHeader (one line)
MAX_CALL_SITES = 4096            # src/analysis/analysis.cpp
SUMMARY_VIRT_MIB = 9.4           # sizeof(CallSiteSummary)=2408 B * MAX_CALL_SITES,
                                 # mmap'd (was 256 MiB before the sparse histogram)

# Default trampoline cost (ns/call) from bench/bench_micro_jit_direct.cpp.
# This is the JIT fast-path inner loop; refresh with --ns-per-call after
# re-running the microbench on the target runner.
DEFAULT_NS_PER_CALL = 2.47


def parse_manifest(path):
    """{(workload, allocator): {'wall_ms':int,'max_rss_kb':int,'exit':int}}."""
    rows = {}
    if not os.path.isfile(path):
        return rows
    with open(path) as f:
        header = f.readline()  # workload allocator wall_ms max_rss_kb exit
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 5:
                continue
            wl, alloc, wall, rss, ex = parts[:5]
            try:
                rows[(wl, alloc)] = {
                    "wall_ms": int(wall), "max_rss_kb": int(rss), "exit": int(ex)}
            except ValueError:
                continue
    return rows


def build_rows(rwdir):
    manifest = parse_manifest(os.path.join(rwdir, "manifest.tsv"))
    dumps = load_dumps(os.path.join(rwdir, "json"))
    workloads = sorted({wl for (wl, _) in manifest} | set(dumps))
    rows = []
    for wl in workloads:
        g = manifest.get((wl, "glibc"))
        t = manifest.get((wl, "tbjit"))
        d = dumps.get(wl, {})
        summ = d.get("summary", {})
        compiled = summ.get("compiled", 0)
        jit = summ.get("jit_allocs", 0)
        gen = summ.get("generic_allocs", 0)
        total = jit + gen
        rss_g = g["max_rss_kb"] if g else None
        rss_t = t["max_rss_kb"] if t else None
        rss_delta = (rss_t - rss_g) if (rss_g is not None and rss_t is not None) else None
        wall_g = g["wall_ms"] if g else None
        wall_t = t["wall_ms"] if t else None
        rows.append({
            "workload": wl,
            "rss_glibc_kb": rss_g,
            "rss_tbjit_kb": rss_t,
            "rss_delta_kb": rss_delta,
            "rss_delta_pct": (100.0 * rss_delta / rss_g)
                             if (rss_delta is not None and rss_g) else None,
            "wall_glibc_ms": wall_g,
            "wall_tbjit_ms": wall_t,
            "wall_delta_ms": (wall_t - wall_g)
                             if (wall_g is not None and wall_t is not None) else None,
            "compiled": compiled,
            "code_page_kib": compiled * CODE_PAGE_BYTES / 1024.0,
            "jit_pct": (100.0 * jit / total) if total else 0.0,
        })
    return rows


def _fmt(v, suffix="", nd=0):
    if v is None:
        return "-"
    if nd:
        return f"{v:.{nd}f}{suffix}"
    return f"{v}{suffix}"


def render(rows, ns_per_call, md=True):
    out = []
    bar = "|" if md else ""
    out.append("## tbjit cost-of-analysis overhead\n" if md
               else "=== tbjit cost-of-analysis overhead ===")
    # Per-workload table.
    if md:
        out.append("| workload | glibc RSS (kB) | tbjit RSS (kB) | RSS Δ (kB) | "
                   "RSS Δ % | wall Δ (ms) | compiled | code pages (KiB) | JIT % |")
        out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        if md:
            out.append(
                f"| {r['workload']} | {_fmt(r['rss_glibc_kb'])} | "
                f"{_fmt(r['rss_tbjit_kb'])} | {_fmt(r['rss_delta_kb'])} | "
                f"{_fmt(r['rss_delta_pct'],'%',1)} | {_fmt(r['wall_delta_ms'])} | "
                f"{r['compiled']} | {r['code_page_kib']:.1f} | {r['jit_pct']:.1f}% |")
        else:
            out.append(f"{r['workload']:<16} rssΔ={_fmt(r['rss_delta_kb'])}kB "
                       f"wallΔ={_fmt(r['wall_delta_ms'])}ms compiled={r['compiled']}")

    # Aggregates.
    rss_deltas = [r["rss_delta_pct"] for r in rows if r["rss_delta_pct"] is not None]
    rss_deltas.sort()
    median_rss = rss_deltas[len(rss_deltas)//2] if rss_deltas else None
    total_code_kib = sum(r["code_page_kib"] for r in rows)
    total_compiled = sum(r["compiled"] for r in rows)

    out.append("")
    out.append("### Memory accounting\n" if md else "--- memory ---")
    out.append(f"- Compiled code pages: {total_compiled} routines x "
               f"{CODE_PAGE_BYTES} B = {total_code_kib:.1f} KiB total across the suite.")
    out.append(f"- Segment headers: {SEGMENT_HEADER_BYTES} B per live 2-MiB segment "
               "(amortized to ~0 of segment payload).")
    out.append(f"- g_summaries: {SUMMARY_VIRT_MIB} MiB *virtual* "
               f"(sizeof(CallSiteSummary) x {MAX_CALL_SITES}), mmap'd lazily — only "
               "touched call-site slots are resident, so the RSS Δ above is the "
               "honest resident cost, not the virtual size.")
    out.append(f"- Median resident RSS overhead vs glibc: "
               f"{_fmt(median_rss,'%',1)} across {len(rss_deltas)} workloads.")

    out.append("")
    out.append("### Time accounting\n" if md else "--- time ---")
    out.append(f"- Trampoline fast-path cost: ~{ns_per_call:.2f} ns/call "
               "(bench/bench_micro_jit_direct.cpp; pass --ns-per-call to refresh).")
    out.append("- Per-workload wall Δ (tbjit vs glibc) is in the table above; it "
               "folds in the LD_PRELOAD interposition + tracing, not just the "
               "fast path.")
    return "\n".join(out)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--ns-per-call", type=float, default=DEFAULT_NS_PER_CALL)
    ap.add_argument("--format", choices=["md", "text"], default="md")
    args = ap.parse_args(argv[1:])
    if not os.path.isdir(args.dir):
        print(f"# not a directory: {args.dir}", file=sys.stderr)
        return 2
    rows = build_rows(args.dir)
    print(render(rows, args.ns_per_call, md=(args.format == "md")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
