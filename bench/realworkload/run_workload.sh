#!/usr/bin/env bash
# Run one real-workload spec under all four allocators (glibc baseline,
# mimalloc, jemalloc, tbjit). Captures per-run wall time, max RSS, exit
# status, stdout, stderr, and (for tbjit only) the picker JSON dump.
#
# Usage:
#   bench/realworkload/run_workload.sh <workload-spec.sh> [out-dir]
#
# A workload spec is a small bash file that sets:
#   WORKLOAD_NAME=...            short stable identifier, becomes dir name
#   WORKLOAD_DESC="..."          one-line human-readable description
#   workload_preconditions       function: returns 0 if runnable, prints
#                                a skip reason on stderr and returns non-zero
#                                if any dependency is missing
#   workload_setup               (optional) function: prep state (tempdir,
#                                clone fixture repo, etc.) — runs once per
#                                workload, before the per-allocator runs.
#                                May export env vars consumed by workload_cmd.
#   workload_cmd                 function: prints the command (with args) to
#                                exec. Must NOT exec itself; this script
#                                handles process launch. The simplest form:
#                                   workload_cmd() { echo "/bin/find /usr -name '*.so'"; }
#   workload_teardown            (optional) function: cleanup after all runs.
#
# Exit code is 0 if at least one allocator run succeeded. Exits 2 if the
# preconditions check fails (i.e. the workload is unavailable on this host).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=bench/realworkload/lib.sh
source "$SCRIPT_DIR/lib.sh"

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <workload-spec.sh> [out-dir]" >&2
  exit 1
fi

SPEC="$1"
OUT_DIR=$(init_out_dir "${2:-}")

if [[ ! -f "$SPEC" ]]; then
  echo "spec not found: $SPEC" >&2
  exit 1
fi

# Reset spec hooks so a stale earlier source doesn't leak through.
WORKLOAD_NAME=""
WORKLOAD_DESC=""
unset -f workload_preconditions workload_setup workload_cmd workload_teardown 2>/dev/null || true

# shellcheck source=/dev/null
source "$SPEC"

if [[ -z "$WORKLOAD_NAME" ]]; then
  echo "spec did not set WORKLOAD_NAME: $SPEC" >&2
  exit 1
fi
if ! declare -F workload_cmd >/dev/null; then
  echo "spec did not define workload_cmd: $SPEC" >&2
  exit 1
fi

# Preconditions: gives the spec a chance to bail out cleanly (e.g.
# "gcc not installed", "redis-server not running"). A missing tool
# isn't a CI failure — it's a skip.
if declare -F workload_preconditions >/dev/null; then
  if ! workload_preconditions; then
    echo "skip $WORKLOAD_NAME: precondition failed" >&2
    exit 2
  fi
fi

W_DIR="$OUT_DIR/$WORKLOAD_NAME"
mkdir -p "$W_DIR/json"

if declare -F workload_setup >/dev/null; then
  workload_setup
fi

# Workloads define the command as a single shell-ready string. Doing it
# this way (rather than an argv array) lets specs use shell features —
# pipes, redirections to /dev/null, env vars set inline — without
# requiring the spec author to understand bash array quoting.
CMD_STR="$(workload_cmd)"
echo "[$WORKLOAD_NAME] cmd: $CMD_STR"

ALLOCATORS=(glibc mimalloc jemalloc tbjit)
ANY_OK=0

for kind in "${ALLOCATORS[@]}"; do
  lib=""
  if ! lib=$(resolve_allocator "$kind"); then
    echo "[$WORKLOAD_NAME] skip $kind: not available" >&2
    continue
  fi

  echo "[$WORKLOAD_NAME] run $kind"
  stdout_path="$W_DIR/$kind.stdout"
  stderr_path="$W_DIR/$kind.stderr"
  time_path="$W_DIR/$kind.time"

  # Build the env preamble for this run. tbjit needs TBJIT_DUMP +
  # TBJIT_DUMP_JSON so analyze_dumps.py can pick up the picker state;
  # the other allocators only need LD_PRELOAD (empty for glibc).
  declare -a env_kv=()
  if [[ -n "$lib" ]]; then
    env_kv+=("LD_PRELOAD=$lib")
  fi
  if [[ "$kind" == "tbjit" ]]; then
    env_kv+=("TBJIT_DUMP=1"
             "TBJIT_DUMP_JSON=$W_DIR/json/$WORKLOAD_NAME.json")
  fi

  # env "$@" path keeps LD_PRELOAD limited to the cmd, not bash itself.
  # `exec $CMD_STR` inside the bash -c is critical: without it, bash
  # interprets the command string but stays around until the child
  # exits, so its own tbjit_fini fires *after* the workload's and
  # overwrites TBJIT_DUMP_JSON with bash's much smaller dump. With
  # `exec`, bash replaces itself with the workload — only one process
  # owns the dump path and the JSON reflects the real workload.
  rc=0
  time_cmd "$stdout_path" "$stderr_path" "$time_path" -- \
    env "${env_kv[@]}" bash -c "exec $CMD_STR" || rc=$?
  append_manifest "$OUT_DIR" "$WORKLOAD_NAME" "$kind" "$time_path"

  if [[ $rc -eq 0 ]]; then
    ANY_OK=1
  else
    echo "[$WORKLOAD_NAME] $kind exited $rc" >&2
  fi
done

if declare -F workload_teardown >/dev/null; then
  workload_teardown
fi

if [[ $ANY_OK -eq 0 ]]; then
  echo "[$WORKLOAD_NAME] all allocator runs failed" >&2
  exit 1
fi
