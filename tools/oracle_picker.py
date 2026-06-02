#!/usr/bin/env python3
"""Summary-derived oracle upper bound for JIT allocation fraction.

NOT an event-stream replay (the JSON dumps are terminal summaries, not
event logs). Models a perfect offline picker that (a) never pays warm-up
cost and (b) never blacklists. A site "captures" all its events if it
ever determined a candidate strategy (compiled or blacklisted at exit).

Both `online%` and `oracle%` are expressed in the same unit (sum of a
site's `events`), so the `gap` is internally consistent. Note this is a
proxy for the run-integrated jit fraction (which the picker reports from
the global jit/generic counters), not that exact number; it is a
defensible *upper bound* on capturable allocation, labeled as such.

Usage: tools/oracle_picker.py <dir-of-json-dumps>
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_dumps import load_dumps


def oracle(dumps):
    rows = []
    for wl, d in sorted(dumps.items()):
        total = sum(s["events"] for s in d["sites"])
        # A perfect picker captures any site that reached a strategy
        # decision, regardless of online blacklisting or warm-up. A
        # blacklisted site's phase is "Blacklisted" (not "Compiled"), so
        # the two sets are disjoint and captured >= online by construction.
        captured = sum(
            s["events"] for s in d["sites"]
            if s["phase"] == "Compiled" or s["blacklisted"]
        )
        online = sum(
            s["events"] for s in d["sites"] if s["phase"] == "Compiled"
        )
        rows.append((wl, total, online, captured))
    return rows


def main():
    if len(sys.argv) < 2:
        print("usage: oracle_picker.py <dir-of-json-dumps>", file=sys.stderr)
        return 1
    dumps = load_dumps(sys.argv[1])
    if not dumps:
        print(f"# no JSON files found under {sys.argv[1]}", file=sys.stderr)
        return 1
    print(f"{'workload':<18}{'total':>10}{'online%':>10}{'oracle%':>10}{'gap':>8}")
    for wl, total, online, cap in oracle(dumps):
        op = 100 * online / total if total else 0.0
        cp = 100 * cap / total if total else 0.0
        print(f"{wl:<18}{total:>10}{op:>9.1f}%{cp:>9.1f}%{cp-op:>7.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
