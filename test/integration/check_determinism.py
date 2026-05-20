#!/usr/bin/env python3
"""Run a target binary twice under tbjit; assert picker decisions match.

Invoked by CTest as the determinism harness for Phase 3a.1 of the paper
roadmap. The picker is supposed to be deterministic over the same trace
— if it isn't, the paper's reproducibility claims are in trouble. This
script captures the strategy + lifetime histograms (not the per-site
addresses, which shift under ASLR) from two TBJIT_DUMP_JSON dumps and
fails non-zero if they disagree.

Usage:
  check_determinism.py <libtbjit.so> <binary> [more-args-to-bin...]

Exit codes:
  0   strategy + lifetime counts match across two runs
  1   counts disagree (with a diff printed to stderr)
  2   harness error (binary missing, dump missing, etc.)
"""

import json
import os
import subprocess
import sys
import tempfile


def categorical_fingerprint(dump):
    """Reduce a dump to (strategy_counts, lifetime_counts, totals).

    These are the picker decisions stripped of any noise that two
    determinism-target runs of the same single-threaded binary could
    legitimately diverge on (timing-of-safe-point, ASLR-shifted return
    addresses, sample-1-in-32 jitter). What matters: "did we pick the
    same strategy for the same workload-shape?"
    """
    summary = dump.get("summary", {})
    sites = dump.get("sites", [])
    compiled = sum(1 for s in sites if s.get("phase") == "Compiled")
    blacklisted = sum(1 for s in sites if s.get("blacklisted"))
    return {
        "strategy_counts":   dict(summary.get("strategy_counts", {})),
        "lifetime_counts":   dict(summary.get("lifetime_counts", {})),
        "compiled_count":    compiled,
        "blacklisted_count": blacklisted,
        # observed_total counts ALL sites (including Observing/PreSpec) —
        # the picker may legitimately churn through some count before
        # locking in but the final count should match.
        "observed_total":    summary.get("sites_observed", 0),
    }


def run_once(libtbjit, binary, args, dump_path):
    env = dict(os.environ,
               LD_PRELOAD=libtbjit,
               TBJIT_DUMP="1",
               TBJIT_DUMP_JSON=dump_path)
    rc = subprocess.run([binary] + args, env=env,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL).returncode
    if rc != 0:
        print(f"# binary exited {rc}", file=sys.stderr)
        return None
    try:
        with open(dump_path) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"# failed to read dump {dump_path}: {e}", file=sys.stderr)
        return None


def diff_fingerprints(a, b):
    diffs = []
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            diffs.append((k, a.get(k), b.get(k)))
    return diffs


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    libtbjit, binary, *args = argv[1:]
    if not os.path.isfile(libtbjit) or not os.access(binary, os.X_OK):
        print(f"# missing libtbjit or binary not executable", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        a_path = os.path.join(tmp, "run_a.json")
        b_path = os.path.join(tmp, "run_b.json")
        dump_a = run_once(libtbjit, binary, args, a_path)
        dump_b = run_once(libtbjit, binary, args, b_path)
        if dump_a is None or dump_b is None:
            return 2

        fa = categorical_fingerprint(dump_a)
        fb = categorical_fingerprint(dump_b)

        diffs = diff_fingerprints(fa, fb)
        if not diffs:
            print("# determinism: OK")
            print(f"#   strategy_counts={fa['strategy_counts']}")
            print(f"#   lifetime_counts={fa['lifetime_counts']}")
            print(f"#   compiled={fa['compiled_count']} "
                  f"blacklisted={fa['blacklisted_count']}")
            return 0
        print("# determinism: FAIL — picker decisions differ between runs",
              file=sys.stderr)
        for k, va, vb in diffs:
            print(f"#   {k}: run_a={va} run_b={vb}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
