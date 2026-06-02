#!/usr/bin/env python3
"""Phase 1/3 paper figures. Reads JSON dumps; writes PDFs to bench-out/figures/.

Requires: matplotlib (no numpy needed). Install in a venv if the system
Python is externally managed (PEP 668):
    python3 -m venv .venv && .venv/bin/pip install matplotlib

Usage:
  plot_phase1.py <json-dir> [out-dir]
                 [--persist DIR] [--sweeps TSV [TSV ...]]

Figures:
  (a) jit_fraction.pdf            per-workload JIT% (from summary jit/generic
                                  counters). With --persist (a dir of run1..N
                                  subdirs), adds mean +/- std error bars from
                                  the per-run spread.
  (b) failure_taxonomy.pdf        blacklisted sites by failure mode, read from
                                  the real deopt_reason codes in the dumps
                                  (falls back to strategy inference for old
                                  dumps); no longer hard-coded at n=4.
  (c) events_to_specialize_cdf.pdf  CDF of first_compile_events over compiled
                                  sites (now multi-point across 12 workloads).
  (d) sensitivity_<knob>.pdf      one per --sweeps TSV: compiled% / blacklist%
                                  / jit% vs the swept knob value.
"""
import argparse
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from analyze_dumps import load_dumps, failure_mode_aggregate  # noqa: E402


def _jit_pct(dump):
    s = dump.get("summary", {})
    jit = s.get("jit_allocs", 0)
    total = jit + s.get("generic_allocs", 0)
    return 100.0 * jit / total if total else 0.0


def _run_dumps(persist_dir):
    """[ {workload: jit_pct} ] for each run subdir under persist_dir."""
    runs = []
    for name in sorted(os.listdir(persist_dir)):
        sub = os.path.join(persist_dir, name)
        if not os.path.isdir(sub):
            continue
        jdir = os.path.join(sub, "json")
        d = load_dumps(jdir if os.path.isdir(jdir) else sub)
        if d:
            runs.append({w: _jit_pct(dump) for w, dump in d.items()})
    return runs


def fig_jit_fraction(dumps, out, persist_dir=None):
    items = sorted(dumps.items())
    names = [w for w, _ in items]
    fracs = [_jit_pct(d) for _, d in items]

    errs = None
    if persist_dir:
        runs = _run_dumps(persist_dir)
        if len(runs) >= 2:
            fracs, errs = [], []
            for w in names:
                vals = [r[w] for r in runs if w in r]
                fracs.append(statistics.mean(vals) if vals else 0.0)
                errs.append(statistics.pstdev(vals) if len(vals) > 1 else 0.0)

    fig, ax = plt.subplots()
    ax.bar(names, fracs, yerr=errs, capsize=3 if errs else 0)
    ax.set_ylabel("JIT allocation fraction (%)")
    ax.set_title("Per-workload JIT yield"
                 + (" (mean +/- std, N runs)" if errs else ""))
    plt.xticks(rotation=30, ha="right")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "jit_fraction.pdf"))
    plt.close(fig)


def fig_failure_taxonomy(dumps, out):
    # Real counts from the ground-truth deopt_reason codes (with strategy
    # fallback for pre-reason dumps), via the shared analyzer aggregation.
    counts = failure_mode_aggregate(dumps)
    if not counts:
        return
    labels = [f"{label}\n({n})" for (label, _expl), n in counts.most_common()]
    vals = [n for _, n in counts.most_common()]
    total = sum(vals)
    fig, ax = plt.subplots()
    ax.barh(labels, vals)
    ax.invert_yaxis()
    ax.set_xlabel("blacklisted sites")
    ax.set_title(f"Failure-mode taxonomy (n={total}, ground-truth reasons)")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "failure_taxonomy.pdf"))
    plt.close(fig)


def fig_etc_cdf(dumps, out):
    vals = sorted(
        s["first_compile_events"]
        for d in dumps.values() for s in d["sites"]
        if s["phase"] == "Compiled" and s.get("first_compile_events", 0) > 0
    )
    if not vals:
        return
    n = len(vals)
    y = [(i + 1) / n for i in range(n)]
    fig, ax = plt.subplots()
    ax.step(vals, y, where="post")
    ax.set_xlabel("events to first compile")
    ax.set_ylabel("CDF")
    ax.set_title(f"Convergence latency (n={n} compiled sites)")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "events_to_specialize_cdf.pdf"))
    plt.close(fig)


def _read_sweep_tsv(path):
    """Aggregate a long-format sweep-summary.tsv by knob value.

    Schema (bench/realworkload/sweep.sh):
      value  workload  sites  compiled  blacklisted  jit_pct   (one row per
    value x workload). Returns sorted [(value, compiled_sum, blacklist_sum,
    jit_mean)] aggregating across workloads at each value.
    """
    by_value = {}  # value -> [compiled_sum, blacklist_sum, [jit_pct...]]
    with open(path) as f:
        header = f.readline().rstrip("\n").split("\t")
        idx = {h: i for i, h in enumerate(header)}
        for ln in f:
            r = ln.rstrip("\n").split("\t")
            if len(r) < len(header):
                continue
            try:
                v = float(r[idx["value"]])
                comp = int(r[idx["compiled"]])
                bl = int(r[idx["blacklisted"]])
                jit = float(r[idx["jit_pct"]])
            except (ValueError, KeyError):
                continue
            acc = by_value.setdefault(v, [0, 0, []])
            acc[0] += comp
            acc[1] += bl
            acc[2].append(jit)
    rows = []
    for v in sorted(by_value):
        comp, bl, jits = by_value[v]
        rows.append((v, comp, bl, statistics.mean(jits) if jits else 0.0))
    return rows


def fig_sensitivity(tsv_path, out):
    rows = _read_sweep_tsv(tsv_path)
    if not rows:
        print(f"# skip sensitivity: no rows in {tsv_path}", file=sys.stderr)
        return
    # Knob name from the artifact dir/file (e.g. sweep-window -> window).
    base = os.path.basename(os.path.dirname(tsv_path)) or os.path.basename(tsv_path)
    knob = base.replace("sweep-", "").split("-")[0] or "knob"

    xs = [r[0] for r in rows]
    compiled = [r[1] for r in rows]
    blacklisted = [r[2] for r in rows]
    jit = [r[3] for r in rows]

    fig, ax = plt.subplots()
    ax.plot(xs, compiled, marker="o", color="tab:blue", label="compiled sites")
    ax.plot(xs, blacklisted, marker="s", color="tab:red", label="blacklisted sites")
    ax.set_xlabel(knob)
    ax.set_ylabel("site count (summed over workloads)")
    ax2 = ax.twinx()
    ax2.plot(xs, jit, marker="^", color="tab:green", label="mean JIT %")
    ax2.set_ylabel("mean JIT allocation fraction (%)")
    lines, labels = ax.get_legend_handles_labels()
    l2, lab2 = ax2.get_legend_handles_labels()
    ax.legend(lines + l2, labels + lab2, loc="best", fontsize=8)
    ax.set_title(f"Sensitivity to {knob}")
    fig.tight_layout()
    fig.savefig(os.path.join(out, f"sensitivity_{knob}.pdf"))
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json_dir")
    ap.add_argument("out_dir", nargs="?", default="bench-out/figures")
    ap.add_argument("--persist",
                    help="dir of run1..N subdirs for JIT-fraction error bars")
    ap.add_argument("--sweeps", nargs="*", default=[],
                    help="sweep-summary.tsv files, one sensitivity figure each")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    dumps = load_dumps(args.json_dir)
    if not dumps:
        print(f"# no JSON files found under {args.json_dir}", file=sys.stderr)
        return 1
    fig_jit_fraction(dumps, args.out_dir, args.persist)
    fig_failure_taxonomy(dumps, args.out_dir)
    fig_etc_cdf(dumps, args.out_dir)
    for tsv in args.sweeps:
        fig_sensitivity(tsv, args.out_dir)
    print(f"wrote figures to {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
