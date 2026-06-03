#!/usr/bin/env python3
"""Memory-frontier figure (RSS vs per-alloc latency) from the knob sweep.

Reads the sweep_results.tsv emitted by bench/realworkload/bound_sweep.sh and,
per workload, plots every (HIST_CAP, SEG_SHIFT, REAP_MODE) configuration as a
point in (peak RSS, p50 latency) space, draws the Pareto frontier (lower-left is
better), and marks the bound baseline (CAP=64, SHIFT=21, conservative) and the
glibc reference.

We deliberately do NOT crown a single optimum (n=3 workloads would overfit);
the figure is the deliverable. To stdout we print, per workload, the Pareto set
and a practical "knee": the config with the largest RSS reduction vs the bound
baseline that keeps p50 within 5% of it.

Requires: matplotlib (no numpy). Usage:
  plot_sweep.py <sweep_results.tsv> [out-dir]
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

BASELINE = ("64", "21", "conservative")
KNEE_TOL = 0.05  # p50 within +5% of bound baseline


def read(path):
    """workload -> list of row dicts; plus glibc[workload] = (rss, p50)."""
    by_wl = {}
    glibc = {}
    order = []
    with open(path) as f:
        header = f.readline().rstrip("\n").split("\t")
        idx = {h: i for i, h in enumerate(header)}
        for ln in f:
            r = ln.rstrip("\n").split("\t")
            if len(r) < len(header):
                continue
            wl = r[idx["workload"]]
            try:
                p50 = float(r[idx["p50_ns"]])
                rss = float(r[idx["peak_rss_kb"]])
            except ValueError:
                continue
            if r[idx["backend"]] == "glibc":
                glibc[wl] = (rss, p50)
                continue
            if wl not in by_wl:
                by_wl[wl] = []
                order.append(wl)
            by_wl[wl].append({
                "cap": r[idx["hist_cap"]],
                "shift": r[idx["seg_shift"]],
                "reap": r[idx["reap_mode"]],
                "p50": p50, "rss": rss,
                "js": r[idx["jit_served"]],
            })
    return order, by_wl, glibc


def pareto(points):
    """Non-dominated points minimizing both rss and p50, sorted by rss."""
    pts = sorted(points, key=lambda d: (d["rss"], d["p50"]))
    front, best_p50 = [], float("inf")
    for d in pts:
        if d["p50"] < best_p50 - 1e-9:
            front.append(d)
            best_p50 = d["p50"]
    return front


def tag(d):
    return f"c{d['cap']}/s{d['shift']}/{d['reap'][:4]}"


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    tsv = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "bench-out/figures"
    os.makedirs(out_dir, exist_ok=True)

    order, by_wl, glibc = read(tsv)
    if not order:
        print(f"# no bound rows in {tsv}", file=sys.stderr)
        return 1

    n = len(order)
    fig, axes = plt.subplots(1, n, figsize=(5 * n, 4.2), squeeze=False)
    for ax, wl in zip(axes[0], order):
        pts = by_wl[wl]
        ax.scatter([d["rss"] for d in pts], [d["p50"] for d in pts],
                   s=28, color="tab:blue", alpha=0.7, label="configs")
        # Pareto frontier.
        fr = pareto(pts)
        if len(fr) >= 2:
            ax.plot([d["rss"] for d in fr], [d["p50"] for d in fr],
                    color="tab:green", lw=1.5, label="Pareto frontier")
        # Baseline + glibc markers.
        base = next((d for d in pts
                     if (d["cap"], d["shift"], d["reap"]) == BASELINE), None)
        if base:
            ax.scatter([base["rss"]], [base["p50"]], s=90, marker="s",
                       facecolors="none", edgecolors="black", label="bound baseline")
        if wl in glibc:
            gr, gp = glibc[wl]
            ax.scatter([gr], [gp], s=120, marker="*", color="tab:red",
                       label="glibc")
        for d in fr:  # annotate frontier configs
            ax.annotate(tag(d), (d["rss"], d["p50"]), fontsize=6,
                        xytext=(3, 3), textcoords="offset points")
        ax.set_xscale("log")
        ax.set_xlabel("peak RSS footprint (kB, log)")
        ax.set_ylabel("per-alloc p50 (ns)")
        ax.set_title(wl)
        ax.legend(fontsize=7)
    fig.tight_layout()
    out = os.path.join(out_dir, "mem_frontier.pdf")
    fig.savefig(out)
    plt.close(fig)
    print(f"wrote {out}")

    # ---- text summary: Pareto set + practical knee per workload ----
    for wl in order:
        pts = by_wl[wl]
        base = next((d for d in pts
                     if (d["cap"], d["shift"], d["reap"]) == BASELINE), None)
        print(f"\n## {wl}")
        if wl in glibc:
            gr, gp = glibc[wl]
            print(f"  glibc reference: rss={gr:.0f} kB  p50={gp:.2f} ns")
        if base:
            print(f"  bound baseline ({tag(base)}): "
                  f"rss={base['rss']:.0f} kB  p50={base['p50']:.2f} ns")
        print("  Pareto frontier (rss kB -> p50 ns):")
        for d in pareto(pts):
            print(f"    {tag(d):>22}  rss={d['rss']:>8.0f}  "
                  f"p50={d['p50']:>7.2f}  jit_served={d['js']}")
        if base:
            cap_p50 = base["p50"] * (1 + KNEE_TOL)
            ok = [d for d in pts if d["p50"] <= cap_p50]
            knee = min(ok, key=lambda d: d["rss"]) if ok else None
            if knee:
                cut = (1 - knee["rss"] / base["rss"]) * 100 if base["rss"] else 0
                print(f"  knee (max RSS cut within +{int(KNEE_TOL*100)}% p50): "
                      f"{tag(knee)}  rss={knee['rss']:.0f} kB "
                      f"({cut:.0f}% lower)  p50={knee['p50']:.2f} ns")
    return 0


if __name__ == "__main__":
    sys.exit(main())
