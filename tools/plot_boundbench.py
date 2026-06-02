#!/usr/bin/env python3
"""Bound-replay figures (Comparison A). Reads the bound_results.tsv emitted by
bench/realworkload/bound_bench_suite.sh; writes PDFs to bench-out/figures/.

Requires: matplotlib (no numpy). Install in a venv if the system Python is
externally managed (PEP 668):
    python3 -m venv .venv && .venv/bin/pip install matplotlib

Usage:
  plot_boundbench.py <bound_results.tsv> [out-dir] [--manifest manifest.tsv]

Figures:
  bound_speedup.pdf   grouped bars: per-allocation p50 latency (ns) by backend
                      (glibc / bound / sim) per workload, with 95% CI error bars.
                      The headline allocator-level comparison.
  bound_rss.pdf       grouped bars: peak RSS delta (kB) by backend per workload.

With --manifest (the real-workload manifest.tsv), prints the Comparison-B
deltas (LD_PRELOAD'd tbjit vs glibc wall + RSS per workload) so the writeup can
quote the interposition tax = B overhead - A bound speedup.
"""
import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

BACKENDS = ["glibc", "bound", "sim"]
COLORS = {"glibc": "tab:gray", "bound": "tab:green", "sim": "tab:orange"}


def read_results(path):
    """{workload: {backend: dict(p50, ci95, rss, mf)}}, preserving workload order."""
    rows = {}
    order = []
    with open(path) as f:
        header = f.readline().rstrip("\n").split("\t")
        idx = {h: i for i, h in enumerate(header)}
        for ln in f:
            r = ln.rstrip("\n").split("\t")
            if len(r) < len(header):
                continue
            backend = r[idx["backend"]]
            wl = r[idx["workload"]]
            if wl not in rows:
                rows[wl] = {}
                order.append(wl)
            rows[wl][backend] = {
                "p50": float(r[idx["p50_ns"]]),
                "ci95": float(r[idx["ci95_ns"]]),
                "rss": float(r[idx["peak_rss_kb"]]),
                "mf": float(r[idx["matched_free_frac"]]),
            }
    return order, rows


def _grouped(order, rows, field, errfield, ylabel, title, out_path):
    present = [b for b in BACKENDS if any(b in rows[w] for w in order)]
    if not order or not present:
        print(f"# skip {out_path}: no data", file=sys.stderr)
        return
    n = len(present)
    width = 0.8 / n
    fig, ax = plt.subplots()
    for i, b in enumerate(present):
        xs = [j + (i - (n - 1) / 2) * width for j in range(len(order))]
        ys = [rows[w].get(b, {}).get(field, 0.0) for w in order]
        errs = ([rows[w].get(b, {}).get(errfield, 0.0) for w in order]
                if errfield else None)
        ax.bar(xs, ys, width, yerr=errs, capsize=3 if errfield else 0,
               label=b, color=COLORS.get(b))
    ax.set_xticks(range(len(order)))
    ax.set_xticklabels(order, rotation=20, ha="right")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def fig_speedup(order, rows, out):
    _grouped(order, rows, "p50", "ci95",
             "per-alloc p50 latency (ns)",
             "Bound-replay latency by backend (Comparison A)",
             os.path.join(out, "bound_speedup.pdf"))
    # Headline ratios + fidelity, to stdout for the writeup.
    for w in order:
        g = rows[w].get("glibc", {}).get("p50")
        b = rows[w].get("bound", {}).get("p50")
        mf = rows[w].get("bound", {}).get("mf")
        if g and b:
            speedup = g / b if b else float("nan")
            print(f"speedup {w}: glibc={g:.2f} bound={b:.2f} ns/alloc "
                  f"-> {speedup:.2f}x  (matched_free_frac={mf})")


def fig_rss(order, rows, out):
    _grouped(order, rows, "rss", None,
             "peak RSS delta (kB)",
             "Bound-replay footprint by backend",
             os.path.join(out, "bound_rss.pdf"))


def comparison_b(manifest_path, order):
    """Print LD_PRELOAD'd tbjit vs glibc wall + RSS per workload (Comparison B)."""
    wall = {}  # (workload, allocator) -> wall_ms
    rss = {}
    with open(manifest_path) as f:
        header = f.readline().rstrip("\n").split("\t")
        idx = {h: i for i, h in enumerate(header)}
        for ln in f:
            r = ln.rstrip("\n").split("\t")
            if len(r) < len(header):
                continue
            wl, al = r[idx["workload"]], r[idx["allocator"]]
            try:
                wall[(wl, al)] = float(r[idx["wall_ms"]])
                rss[(wl, al)] = float(r[idx["max_rss_kb"]])
            except ValueError:
                continue
    print("\n# Comparison B (LD_PRELOAD'd tbjit vs glibc, from manifest.tsv):")
    for w in order:
        gw, tw = wall.get((w, "glibc")), wall.get((w, "tbjit"))
        gr, tr = rss.get((w, "glibc")), rss.get((w, "tbjit"))
        if gw and tw:
            print(f"  {w}: wall tbjit/glibc = {tw:.0f}/{gw:.0f} ms "
                  f"({tw/gw:.2f}x)" + (
                      f"  rss {tr/gr:.2f}x" if gr and tr else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_tsv")
    ap.add_argument("out_dir", nargs="?", default="bench-out/figures")
    ap.add_argument("--manifest", help="real-workload manifest.tsv for Comparison B")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    order, rows = read_results(args.results_tsv)
    if not order:
        print(f"# no rows in {args.results_tsv}", file=sys.stderr)
        return 1
    fig_speedup(order, rows, args.out_dir)
    fig_rss(order, rows, args.out_dir)
    if args.manifest and os.path.exists(args.manifest):
        comparison_b(args.manifest, order)
    print(f"wrote figures to {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
