#!/usr/bin/env bash
# Shared helpers for the real-workload bench runner.
#
# Sourced by run_workload.sh and run_suite.sh. Locates the four
# allocators of interest (glibc baseline, mimalloc, jemalloc, tbjit),
# provides a uniform `time_cmd` wrapper around `/usr/bin/time -v` that
# captures wall + max-rss into a parseable single-line summary, and
# defines the LD_PRELOAD envelope for each allocator.
#
# Why bash + /usr/bin/time instead of a Python harness:
#   - The real-workload binaries we care about (gcc, sqlite3, redis, …)
#     are launched by the OS, not from inside a Python process, so the
#     measurement boundary is the process; /usr/bin/time is the
#     canonical, low-overhead way to measure that boundary on Linux.
#   - bash keeps the LD_PRELOAD plumbing trivial — Python would have to
#     re-implement the same dance via subprocess.Popen(env=...).
#
# We do NOT measure per-call latency here. The synthetic benches in
# bench/*.cpp already do that with cycle-counter precision. The real-
# workload runner answers a different question: do real programs
# end-to-end change under each allocator, and what does the picker do
# when fed real allocation traces?

set -euo pipefail

# ---- repo + build dir resolution ------------------------------------------

# REPO_ROOT is the tbjit-alloc repo root. Each script sources lib.sh from a
# path relative to itself; resolve the repo root from this file's location.
REALWORKLOAD_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$REALWORKLOAD_LIB_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

# ---- allocator library resolution -----------------------------------------

# All four allocators are referenced as a "kind" (glibc/mimalloc/jemalloc/
# tbjit). resolve_allocator <kind> echoes the LD_PRELOAD path (or empty for
# glibc, which is the default and needs no preload). Returns non-zero if
# the requested allocator isn't installed.
resolve_allocator() {
  local kind="$1"
  case "$kind" in
    glibc)
      echo ""
      return 0
      ;;
    tbjit)
      local lib="$BUILD_DIR/libtbjit.so"
      [[ "$(uname)" == "Darwin" ]] && lib="$BUILD_DIR/libtbjit.dylib"
      if [[ ! -f "$lib" ]]; then
        echo "tbjit not built: $lib" >&2
        return 1
      fi
      echo "$lib"
      return 0
      ;;
    mimalloc|jemalloc)
      local pattern="lib${kind}\\.so(\\.[0-9]+)+$"
      local found=""
      if command -v ldconfig >/dev/null 2>&1; then
        found=$(ldconfig -p 2>/dev/null \
          | grep -E "$pattern" \
          | head -1 | awk '{print $NF}' || true)
      fi
      if [[ -z "$found" ]]; then
        echo "$kind not installed (apt: lib${kind}-dev)" >&2
        return 1
      fi
      echo "$found"
      return 0
      ;;
    *)
      echo "unknown allocator: $kind" >&2
      return 1
      ;;
  esac
}

# ---- per-run output layout ------------------------------------------------

# Standard layout under $OUT_DIR (caller-supplied, typically bench-out/
# realworkload/):
#
#   manifest.tsv                            workload allocator wall_ms max_rss_kb exit
#   <workload>/<allocator>.stdout           captured stdout
#   <workload>/<allocator>.stderr           captured stderr (+ time -v output below it)
#   <workload>/<allocator>.time             one-line summary: wall=... rss=... exit=...
#   <workload>/json/<workload>.json         tbjit JSON dump (tbjit allocator only)
#
# The manifest is the canonical machine-readable form; everything else is
# either raw artifact or convenience.

OUT_DIR_DEFAULT="$REPO_ROOT/bench-out/realworkload"

init_out_dir() {
  local out="${1:-$OUT_DIR_DEFAULT}"
  mkdir -p "$out"
  if [[ ! -e "$out/manifest.tsv" ]]; then
    printf "workload\tallocator\twall_ms\tmax_rss_kb\texit\n" > "$out/manifest.tsv"
  fi
  echo "$out"
}

# ---- the measurement primitive --------------------------------------------

# time_cmd <stdout_path> <stderr_path> <time_path> -- <cmd> [args...]
#
# Runs cmd, captures stdout/stderr to the given files, runs it under
# /usr/bin/time -v so we can extract wall + max-rss + exit. Writes a
# single-line summary "wall_ms=… max_rss_kb=… exit=…" to time_path so
# downstream parsing is trivial. Returns the cmd's exit status.
#
# On macOS /usr/bin/time has a different format (and no -v); we degrade
# to a wall-time-only measurement using SECONDS. macOS isn't where we
# expect to run real workloads — CI is Linux — but local development
# shouldn't crash.
time_cmd() {
  local stdout_path="$1" stderr_path="$2" time_path="$3"
  shift 3
  [[ "$1" == "--" ]] && shift

  local exit_status=0
  if [[ "$(uname)" == "Linux" ]] && command -v /usr/bin/time >/dev/null 2>&1; then
    # /usr/bin/time -v writes its report to stderr, and we want it to
    # land in stderr_path AFTER the command's own stderr. Easiest: tee
    # cmd's stderr to a temp, then append time's report.
    local time_report
    time_report=$(mktemp)
    if /usr/bin/time -v -o "$time_report" \
        bash -c "\"\$@\" >\"$stdout_path\" 2>\"$stderr_path\"" \
        _ "$@"; then
      exit_status=0
    else
      exit_status=$?
    fi
    cat "$time_report" >> "$stderr_path"

    local wall_s
    wall_s=$(awk -F': ' '
      /Elapsed \(wall clock\)/ {
        n = split($2, a, ":")
        if (n == 3)      print a[1]*3600 + a[2]*60 + a[3]
        else if (n == 2) print a[1]*60 + a[2]
        else             print a[1]
        exit
      }' "$time_report")
    local rss_kb
    rss_kb=$(awk -F': ' '/Maximum resident set size/ {print $2; exit}' \
              "$time_report")
    rm -f "$time_report"

    local wall_ms
    wall_ms=$(awk -v s="$wall_s" 'BEGIN { printf("%.0f", s * 1000.0) }')
    printf "wall_ms=%s max_rss_kb=%s exit=%s\n" \
      "${wall_ms:-0}" "${rss_kb:-0}" "$exit_status" > "$time_path"
  else
    local t0 t1
    t0=$(python3 -c 'import time;print(time.monotonic())')
    if "$@" >"$stdout_path" 2>"$stderr_path"; then
      exit_status=0
    else
      exit_status=$?
    fi
    t1=$(python3 -c 'import time;print(time.monotonic())')
    local wall_ms
    wall_ms=$(python3 -c "print(int(($t1 - $t0) * 1000))")
    printf "wall_ms=%s max_rss_kb=0 exit=%s\n" \
      "$wall_ms" "$exit_status" > "$time_path"
  fi
  return "$exit_status"
}

# ---- manifest append ------------------------------------------------------

# append_manifest <out_dir> <workload> <allocator> <time_path>
# Pulls the values out of <time_path> and adds a row to manifest.tsv.
append_manifest() {
  local out="$1" workload="$2" allocator="$3" tpath="$4"
  local wall rss exit
  wall=$(awk -F= '/^wall_ms/ {print $2; exit}' "$tpath" | awk '{print $1}')
  rss=$(awk -F= '{for (i=1;i<=NF;i++) if ($i ~ /max_rss_kb/) {print $(i+1); exit}}' \
         "$tpath" | awk '{print $1}')
  exit=$(awk -F= '/exit=/ {n=split($0, a, "exit="); print a[2]; exit}' "$tpath")
  # The awk parses above are intentionally lax; fall back to the regex
  # form if any field comes back empty.
  if [[ -z "$wall" || -z "$rss" || -z "$exit" ]]; then
    # shellcheck disable=SC2155
    local line=$(cat "$tpath")
    wall=$(sed -n 's/.*wall_ms=\([0-9]*\).*/\1/p' <<<"$line")
    rss=$(sed -n 's/.*max_rss_kb=\([0-9]*\).*/\1/p' <<<"$line")
    exit=$(sed -n 's/.*exit=\([0-9]*\).*/\1/p' <<<"$line")
  fi
  printf "%s\t%s\t%s\t%s\t%s\n" \
    "$workload" "$allocator" "${wall:-0}" "${rss:-0}" "${exit:-0}" \
    >> "$out/manifest.tsv"
}
