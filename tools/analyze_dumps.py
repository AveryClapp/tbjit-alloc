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


# Quantile set used in every "distribution" section. Picked to match the
# granularity reviewers expect for CDF-style claims in the paper: a few
# tail points (p90/p95/p99) for "how bad does it get?" and a few middle
# points (p25/p50/p75) for "what does a typical site look like?".
DIST_QUANTILES = [0.10, 0.25, 0.50, 0.75, 0.90, 0.95, 0.99]


def distribution_stats(values):
    """Return a dict of summary stats over `values` (an iterable of numbers).

    Empty inputs return a dict where everything is 0 — formatters can
    then render a one-row "n=0" line rather than crashing on an empty
    workload set. mean/stdev are intentionally float; quantiles match
    the input type so integer-valued samples stay integer-readable.
    """
    s = sorted(values)
    n = len(s)
    stats = {"n": n}
    if n == 0:
        for q in DIST_QUANTILES:
            stats[f"p{int(q*100)}"] = 0
        stats.update(min=0, max=0, mean=0.0, stdev=0.0)
        return stats
    stats["min"] = s[0]
    stats["max"] = s[-1]
    stats["mean"] = statistics.mean(s)
    stats["stdev"] = statistics.stdev(s) if n > 1 else 0.0
    for q in DIST_QUANTILES:
        stats[f"p{int(q*100)}"] = quantile(s, q)
    return stats


def cross_workload_distributions(rows):
    """Per-metric distribution stats across workloads.

    Each metric becomes a row in the output table: "compile rate varied
    from 12% to 91% across N workloads, p50 = 45%." This is the answer
    to roadmap question 1: "Across N workloads, what fraction of call
    sites reach Compiled state?" — but as a *distribution*, not a
    single bucket-mean, because the variance is the paper-interesting
    signal.
    """
    if not rows:
        return {}
    sites = [r["sites"] for r in rows]
    compiled = [r["compiled"] for r in rows]
    blacklisted = [r["blacklisted"] for r in rows]
    compile_rate_pct = [
        100.0 * r["compiled"] / r["sites"] if r["sites"] else 0.0
        for r in rows
    ]
    blacklist_rate_pct = [
        100.0 * r["blacklisted"] / r["sites"] if r["sites"] else 0.0
        for r in rows
    ]
    jit_pct = [r["jit_pct"] for r in rows]
    return {
        "sites_per_workload": distribution_stats(sites),
        "compiled_per_workload": distribution_stats(compiled),
        "blacklisted_per_workload": distribution_stats(blacklisted),
        "compile_rate_pct": distribution_stats(compile_rate_pct),
        "blacklist_rate_pct": distribution_stats(blacklist_rate_pct),
        "jit_alloc_pct": distribution_stats(jit_pct),
    }


# --- formatters ------------------------------------------------------------

_DIST_METRIC_LABELS = {
    "sites_per_workload":      "sites / workload",
    "compiled_per_workload":   "compiled / workload",
    "blacklisted_per_workload":"blacklisted / workload",
    "compile_rate_pct":        "compile rate (%)",
    "blacklist_rate_pct":      "blacklist rate (%)",
    "jit_alloc_pct":           "JIT alloc fraction (%)",
}


def _fmt_dist_value(stats_key, v):
    """Format a stat value for display.

    Percent-valued metrics get one decimal; counts stay integer (cast
    to int so a numpy/statistics float doesn't slip through with .0).
    """
    if isinstance(v, float):
        # mean/stdev or pct distributions
        return f"{v:.1f}"
    return f"{int(v)}"


def fmt_text(rows, strategies, lifetimes, blacklists, fce, dists):
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
        fce_stats = distribution_stats(fce)
        out.append(f"=== Events-to-specialize distribution (n={fce_stats['n']}) ===")
        out.append(
            "  min={min}  p10={p10}  p25={p25}  p50={p50}  p75={p75}  "
            "p90={p90}  p95={p95}  p99={p99}  max={max}".format(**fce_stats)
        )
        out.append(f"  mean={fce_stats['mean']:.0f}  "
                   f"stdev={fce_stats['stdev']:.0f}")
        out.append("")
    if dists:
        out.append("=== Cross-workload distributions ===")
        out.append(f"{'metric':<24} {'n':>3} {'min':>8} {'p25':>8} "
                   f"{'p50':>8} {'p75':>8} {'p95':>8} {'max':>8} "
                   f"{'mean':>8} {'stdev':>8}")
        out.append("-" * 96)
        for key, label in _DIST_METRIC_LABELS.items():
            s = dists.get(key)
            if not s or s["n"] == 0:
                continue
            out.append(
                f"{label:<24} {s['n']:>3} "
                f"{_fmt_dist_value(key, s['min']):>8} "
                f"{_fmt_dist_value(key, s['p25']):>8} "
                f"{_fmt_dist_value(key, s['p50']):>8} "
                f"{_fmt_dist_value(key, s['p75']):>8} "
                f"{_fmt_dist_value(key, s['p95']):>8} "
                f"{_fmt_dist_value(key, s['max']):>8} "
                f"{s['mean']:>8.1f} "
                f"{s['stdev']:>8.1f}"
            )
    return "\n".join(out)


def fmt_md(rows, strategies, lifetimes, blacklists, fce, dists):
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
        fce_stats = distribution_stats(fce)
        out.append(f"## Events-to-specialize distribution (n={fce_stats['n']})")
        out.append("")
        out.append("| stat | value |")
        out.append("|---|---:|")
        for stat in ("min", "p10", "p25", "p50", "p75", "p90", "p95", "p99",
                     "max"):
            out.append(f"| {stat} | {fce_stats[stat]} |")
        out.append(f"| mean | {fce_stats['mean']:.0f} |")
        out.append(f"| stdev | {fce_stats['stdev']:.0f} |")
        out.append("")
    if dists:
        out.append("## Cross-workload distributions")
        out.append("")
        out.append("| metric | n | min | p25 | p50 | p75 | p95 | max | "
                   "mean | stdev |")
        out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for key, label in _DIST_METRIC_LABELS.items():
            s = dists.get(key)
            if not s or s["n"] == 0:
                continue
            out.append(
                f"| {label} | {s['n']} | "
                f"{_fmt_dist_value(key, s['min'])} | "
                f"{_fmt_dist_value(key, s['p25'])} | "
                f"{_fmt_dist_value(key, s['p50'])} | "
                f"{_fmt_dist_value(key, s['p75'])} | "
                f"{_fmt_dist_value(key, s['p95'])} | "
                f"{_fmt_dist_value(key, s['max'])} | "
                f"{s['mean']:.1f} | {s['stdev']:.1f} |"
            )
    return "\n".join(out)


def fmt_tsv(rows, strategies, lifetimes, blacklists, fce, dists):
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
    for metric_key, stats in (dists or {}).items():
        for stat_key, value in stats.items():
            out.append(f"dist:{metric_key}\t{stat_key}\t{value}")
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
    dists = cross_workload_distributions(rows)

    formatters = {"text": fmt_text, "md": fmt_md, "tsv": fmt_tsv}
    print(formatters[args.format](
        rows, strategies, lifetimes, blacklists, fce, dists))
    return 0


if __name__ == "__main__":
    sys.exit(main())
