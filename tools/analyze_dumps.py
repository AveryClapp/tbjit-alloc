#!/usr/bin/env python3
"""Aggregate tbjit JSON dump files into paper-ready summary stats.

Reads every *.json file in a directory (expected format: the output of
dump_stats() with TBJIT_DUMP_JSON= set — see src/analysis/dump.cpp) and
emits per-workload and cross-workload aggregate statistics in one of
three formats:

  - text:  human-readable, printed to stdout
  - md:    GitHub-flavored markdown (drop into the bench job summary)
  - tsv:   tab-separated for downstream analysis pipelines / Python

Usage:
  tools/analyze_dumps.py <dir-of-json-dumps> [--format text|md|tsv]

Designed to be the foundation for paper figure generation: every figure
that aggregates picker behavior across workloads should source its data
from this script's output (or read the JSON directly via this module's
load_dumps()).
"""

import argparse
import json
import os
import statistics
import sys
from collections import Counter


def load_dumps(dir_path):
    """Return {workload_name: parsed_json_dict} for every *.json in dir."""
    dumps = {}
    if not os.path.isdir(dir_path):
        return dumps
    for fname in sorted(os.listdir(dir_path)):
        if not fname.endswith(".json"):
            continue
        path = os.path.join(dir_path, fname)
        try:
            with open(path) as f:
                dumps[fname[:-5]] = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            print(f"# skipping {fname}: {e}", file=sys.stderr)
    return dumps


def per_workload_rows(dumps):
    rows = []
    for name, d in dumps.items():
        s = d.get("summary", {})
        jit = s.get("jit_allocs", 0)
        generic = s.get("generic_allocs", 0)
        total = jit + generic
        rows.append({
            "workload": name,
            "sites": s.get("sites_observed", 0),
            "compiled": s.get("compiled", 0),
            "blacklisted": s.get("blacklisted", 0),
            "deopt": s.get("deopt", 0),
            "prespec": s.get("prespec", 0),
            "jit_allocs": jit,
            "generic_allocs": generic,
            "jit_pct": 100.0 * jit / total if total else 0.0,
            "total_allocs": total,
        })
    return rows


def aggregate_strategies(dumps):
    counts = Counter()
    for d in dumps.values():
        for site in d.get("sites", []):
            if site.get("phase") == "Compiled":
                counts[site.get("strategy", "-")] += 1
    return counts


def aggregate_lifetimes(dumps):
    counts = Counter()
    for d in dumps.values():
        for site in d.get("sites", []):
            if site.get("phase") == "Compiled":
                counts[site.get("lifetime", "Unknown")] += 1
    return counts


def aggregate_blacklist_reasons(dumps):
    """Crude: count blacklisted sites grouped by their candidate strategy.

    The picker doesn't yet record *why* a site was blacklisted (see Phase
    3a.4 in the paper roadmap — failure-mode taxonomy is future work).
    This aggregation gives at least the rough strategy that was last
    attempted before the site gave up.
    """
    counts = Counter()
    for d in dumps.values():
        for site in d.get("sites", []):
            if site.get("blacklisted"):
                counts[site.get("strategy", "-")] += 1
    return counts


def first_compile_distribution(dumps):
    samples = []
    for d in dumps.values():
        for site in d.get("sites", []):
            fce = site.get("first_compile_events", 0)
            if fce > 0:
                samples.append(fce)
    return sorted(samples)


def quantile(sorted_samples, q):
    if not sorted_samples:
        return 0
    idx = min(len(sorted_samples) - 1, int(q * len(sorted_samples)))
    return sorted_samples[idx]


# --- formatters ------------------------------------------------------------

def fmt_text(rows, strategies, lifetimes, blacklists, fce):
    out = []
    out.append("=== Per-workload summary ===")
    out.append(f"{'workload':<22} {'sites':>6} {'cmp':>5} {'bl':>3} "
               f"{'jit%':>7} {'allocs':>12}")
    out.append("-" * 64)
    for r in rows:
        out.append(f"{r['workload']:<22} {r['sites']:>6} {r['compiled']:>5} "
                   f"{r['blacklisted']:>3} {r['jit_pct']:>6.1f}% "
                   f"{r['total_allocs']:>12}")
    out.append("")
    tot_s = sum(strategies.values())
    out.append(f"=== Strategy distribution (aggregate, n={tot_s} compiled sites) ===")
    for strat, n in strategies.most_common():
        pct = 100.0 * n / tot_s if tot_s else 0
        out.append(f"  {strat:<22} {n:>5} ({pct:>5.1f}%)")
    out.append("")
    tot_l = sum(lifetimes.values())
    out.append(f"=== Lifetime distribution (aggregate, n={tot_l}) ===")
    for life, n in lifetimes.most_common():
        pct = 100.0 * n / tot_l if tot_l else 0
        out.append(f"  {life:<22} {n:>5} ({pct:>5.1f}%)")
    out.append("")
    if blacklists:
        out.append("=== Blacklisted sites by last-attempted strategy ===")
        for strat, n in blacklists.most_common():
            out.append(f"  {strat:<22} {n:>5}")
        out.append("")
    if fce:
        out.append(f"=== Events-to-specialize distribution (n={len(fce)}) ===")
        out.append(f"  min={fce[0]}  p50={quantile(fce, 0.50)}  "
                   f"p95={quantile(fce, 0.95)}  max={fce[-1]}")
        if len(fce) > 1:
            out.append(f"  mean={statistics.mean(fce):.0f}  "
                       f"stdev={statistics.stdev(fce):.0f}")
    return "\n".join(out)


def fmt_md(rows, strategies, lifetimes, blacklists, fce):
    out = []
    out.append("## Per-workload summary")
    out.append("")
    out.append("| workload | sites | compiled | blacklisted | deopt | "
               "prespec | jit % | total allocs |")
    out.append("|---|---:|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        out.append(
            f"| {r['workload']} | {r['sites']} | {r['compiled']} | "
            f"{r['blacklisted']} | {r['deopt']} | {r['prespec']} | "
            f"{r['jit_pct']:.1f}% | {r['total_allocs']} |"
        )
    out.append("")
    tot_s = sum(strategies.values())
    out.append(f"## Strategy distribution (aggregate, n={tot_s})")
    out.append("")
    out.append("| strategy | count | % |")
    out.append("|---|---:|---:|")
    for strat, n in strategies.most_common():
        pct = 100.0 * n / tot_s if tot_s else 0
        out.append(f"| {strat} | {n} | {pct:.1f}% |")
    out.append("")
    tot_l = sum(lifetimes.values())
    out.append(f"## Lifetime distribution (aggregate, n={tot_l})")
    out.append("")
    out.append("| lifetime | count | % |")
    out.append("|---|---:|---:|")
    for life, n in lifetimes.most_common():
        pct = 100.0 * n / tot_l if tot_l else 0
        out.append(f"| {life} | {n} | {pct:.1f}% |")
    out.append("")
    if blacklists:
        out.append("## Blacklisted sites by last-attempted strategy")
        out.append("")
        out.append("| strategy | count |")
        out.append("|---|---:|")
        for strat, n in blacklists.most_common():
            out.append(f"| {strat} | {n} |")
        out.append("")
    if fce:
        out.append(f"## Events-to-specialize distribution (n={len(fce)})")
        out.append("")
        out.append("| stat | value |")
        out.append("|---|---:|")
        out.append(f"| min | {fce[0]} |")
        out.append(f"| p50 | {quantile(fce, 0.50)} |")
        out.append(f"| p95 | {quantile(fce, 0.95)} |")
        out.append(f"| max | {fce[-1]} |")
        if len(fce) > 1:
            out.append(f"| mean | {statistics.mean(fce):.0f} |")
            out.append(f"| stdev | {statistics.stdev(fce):.0f} |")
    return "\n".join(out)


def fmt_tsv(rows, strategies, lifetimes, blacklists, fce):
    out = ["section\tkey\tvalue"]
    for r in rows:
        for k, v in r.items():
            if k == "workload":
                continue
            out.append(f"workload:{r['workload']}\t{k}\t{v}")
    for strat, n in strategies.most_common():
        out.append(f"strategy\t{strat}\t{n}")
    for life, n in lifetimes.most_common():
        out.append(f"lifetime\t{life}\t{n}")
    for strat, n in blacklists.most_common():
        out.append(f"blacklist_strategy\t{strat}\t{n}")
    for v in fce:
        out.append(f"fce\t-\t{v}")
    return "\n".join(out)


def main(argv=None):
    p = argparse.ArgumentParser(
        description="Aggregate tbjit JSON dumps into summary stats.")
    p.add_argument("dir", help="directory containing *.json dump files")
    p.add_argument("--format", default="text", choices=["text", "md", "tsv"],
                   help="output format (default: text)")
    args = p.parse_args(argv)

    dumps = load_dumps(args.dir)
    if not dumps:
        print(f"# no JSON files found under {args.dir}", file=sys.stderr)
        return 1

    rows = per_workload_rows(dumps)
    strategies = aggregate_strategies(dumps)
    lifetimes = aggregate_lifetimes(dumps)
    blacklists = aggregate_blacklist_reasons(dumps)
    fce = first_compile_distribution(dumps)

    formatters = {"text": fmt_text, "md": fmt_md, "tsv": fmt_tsv}
    print(formatters[args.format](rows, strategies, lifetimes, blacklists, fce))
    return 0


if __name__ == "__main__":
    sys.exit(main())
