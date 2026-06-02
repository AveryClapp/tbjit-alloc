#!/usr/bin/env python3
"""Cross-run picker persistence: do site IDs and decisions reproduce?

Phase 3a.5 of the paper roadmap. The picker is supposed to be deterministic
over the same workload: the same call sites should appear (return-address
hashes are stable under ASLR-off CI), and each site should get the same
strategy and the same blacklist verdict run after run. If they don't, the
"profile-guided allocator init" future-work bullet is dead on arrival.

This generalizes test/integration/check_determinism.py (which compares two
runs of a single binary by categorical fingerprint) to N runs across the
whole real-workload suite, reporting per-site reproducibility.

Usage:
  persistence.py <dir>

<dir> contains one subdirectory per run (run1, run2, ...). Each run subdir is
either a flat directory of TBJIT_DUMP_JSON files, or a directory containing a
`json/` subdir of them (the layout run_suite.sh produces). Runs are matched by
workload name; only workloads present in *every* run are scored.

Exit codes:
  0  analysis completed (always, unless harness error)
  2  fewer than two runs found
"""

import os
import sys
from collections import defaultdict

# Reuse the dominant-PID grouping logic from the aggregate analyzer.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_dumps import load_dumps  # noqa: E402


def _run_dirs(parent):
    """Return sorted [(run_name, json_dir)] for each run subdir under parent."""
    runs = []
    for name in sorted(os.listdir(parent)):
        sub = os.path.join(parent, name)
        if not os.path.isdir(sub):
            continue
        json_sub = os.path.join(sub, "json")
        runs.append((name, json_sub if os.path.isdir(json_sub) else sub))
    return runs


def _site_index(dump):
    """{site_id: (strategy, blacklisted)} for one workload dump."""
    out = {}
    for s in dump.get("sites", []):
        out[s.get("id")] = (s.get("strategy", "-"), bool(s.get("blacklisted")))
    return out


def _jaccard(sets):
    union = set().union(*sets)
    if not union:
        return 1.0  # vacuously identical: no sites anywhere
    inter = set(sets[0]).intersection(*sets[1:])
    return len(inter) / len(union)


def analyze(parent):
    runs = _run_dirs(parent)
    if len(runs) < 2:
        print(f"# need >=2 run subdirs under {parent}, found {len(runs)}",
              file=sys.stderr)
        return 2

    # {workload: [per-run site_index dict]} for workloads present in all runs.
    per_run = [load_dumps(jd) for _, jd in runs]
    common = set(per_run[0])
    for d in per_run[1:]:
        common &= set(d)

    rows = []
    tot_common_sites = 0
    tot_strategy_agree = 0
    for wl in sorted(common):
        indices = [_site_index(d[wl]) for d in per_run]
        site_sets = [set(ix) for ix in indices]
        jac = _jaccard(site_sets)

        shared = set(indices[0]).intersection(*[set(ix) for ix in indices[1:]])
        n = len(shared)
        strat_agree = sum(
            1 for sid in shared
            if len({ix[sid][0] for ix in indices}) == 1)
        bl_agree = sum(
            1 for sid in shared
            if len({ix[sid][1] for ix in indices}) == 1)

        # blacklist-set stability: Jaccard of the per-run blacklisted-site sets.
        bl_sets = [{sid for sid, (_, bl) in ix.items() if bl} for ix in indices]
        bl_jac = _jaccard(bl_sets)

        rows.append({
            "workload": wl,
            "runs": len(runs),
            "site_jaccard": jac,
            "shared_sites": n,
            "strategy_repro": (strat_agree / n) if n else 1.0,
            "blacklist_repro": (bl_agree / n) if n else 1.0,
            "blacklist_jaccard": bl_jac,
        })
        tot_common_sites += n
        tot_strategy_agree += strat_agree

    print(f"# persistence over {len(runs)} runs: {', '.join(n for n, _ in runs)}")
    print(f"# workloads present in all runs: {len(common)}")
    print()
    hdr = ("| workload | runs | site Jaccard | shared sites | "
           "strategy repro | blacklist repro | blacklist Jaccard |")
    print(hdr)
    print("|---|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        print(f"| {r['workload']} | {r['runs']} | {r['site_jaccard']:.3f} | "
              f"{r['shared_sites']} | {r['strategy_repro']*100:.1f}% | "
              f"{r['blacklist_repro']*100:.1f}% | {r['blacklist_jaccard']:.3f} |")
    print()
    headline = (tot_strategy_agree / tot_common_sites) if tot_common_sites else 1.0
    mean_jac = (sum(r["site_jaccard"] for r in rows) / len(rows)) if rows else 1.0
    print(f"# HEADLINE: picker decisions reproduce in {headline*100:.1f}% of "
          f"shared sites across {len(runs)} runs "
          f"(mean site-ID Jaccard {mean_jac:.3f}).")
    return 0


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    return analyze(argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
