#!/usr/bin/env python3
"""Phase 1 paper figures. Reads JSON dumps; writes PDFs to bench-out/figures/.

Requires: matplotlib (no numpy needed). Install in a venv if the system
Python is externally managed (PEP 668):
    python3 -m venv .venv && .venv/bin/pip install matplotlib

Usage: tools/plot_phase1.py <json-dir> [out-dir]

Three figures:
  (a) jit_fraction.pdf            per-workload JIT% (from summary jit/generic
                                  counters, matching phase1-results.md's 7.1%).
  (b) failure_taxonomy.pdf        the four blacklisted sites by class (n=4,
                                  hard-coded from the classified taxonomy).
  (c) events_to_specialize_cdf.pdf  CDF of first_compile_events over compiled
                                  sites. Sparse in the Phase 1 dataset (only
                                  3 compiled sites); becomes meaningful after
                                  the STABLE_WINDOWS sweep and more workloads.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from analyze_dumps import load_dumps


def fig_jit_fraction(dumps, out):
    # jit% from the global jit/generic counters in the summary — the same
    # run-integrated fraction phase1-results.md reports (gcc ~7.1%), not a
    # per-site phase proxy.
    items = sorted(dumps.items())
    names = [w for w, _ in items]
    fracs = []
    for _, d in items:
        s = d.get("summary", {})
        jit = s.get("jit_allocs", 0)
        total = jit + s.get("generic_allocs", 0)
        fracs.append(100 * jit / total if total else 0.0)
    fig, ax = plt.subplots()
    ax.bar(names, fracs)
    ax.set_ylabel("JIT allocation fraction (%)")
    ax.set_title("Per-workload JIT yield")
    plt.xticks(rotation=30, ha="right")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "jit_fraction.pdf"))
    plt.close(fig)


def fig_failure_taxonomy(out):
    # n=4; hard-coded from the classified taxonomy in phase1-results.md.
    classes = ["Class drift\n(MultiSize x2)",
               "Hold misclass\n(Bump x1)",
               "LIFO violation\n(Paired x1)"]
    counts = [2, 1, 1]
    fig, ax = plt.subplots()
    ax.barh(classes, counts)
    ax.set_xlabel("blacklisted sites")
    ax.set_title("Failure-mode taxonomy (n=4)")
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
    ax.set_title("Convergence latency")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "events_to_specialize_cdf.pdf"))
    plt.close(fig)


def main():
    if len(sys.argv) < 2:
        print("usage: plot_phase1.py <json-dir> [out-dir]", file=sys.stderr)
        return 1
    json_dir = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "bench-out/figures"
    os.makedirs(out, exist_ok=True)
    dumps = load_dumps(json_dir)
    if not dumps:
        print(f"# no JSON files found under {json_dir}", file=sys.stderr)
        return 1
    fig_jit_fraction(dumps, out)
    fig_failure_taxonomy(out)
    fig_etc_cdf(dumps, out)
    print(f"wrote figures to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
