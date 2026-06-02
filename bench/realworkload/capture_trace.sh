#!/usr/bin/env bash
# Capture a complete, unspecialized allocation trace for one workload spec.
#
# Runs the workload under LD_PRELOAD=libtbjit with TBJIT_TRACE_ONLY=1 (forces
# every alloc through the generic record path, so the trace is the full event
# stream — no JIT'd allocs go unrecorded) and TBJIT_TRACE=<dir>/<name>.%p.trace
# (one file per process; %p is the PID, so forked children don't clobber each
# other). setarch -R disables ASLR so return-address-hashed site ids are stable
# across the capture and any later re-run.
#
# Usage: capture_trace.sh <workload-spec.sh> <trace-out-dir>
# Exit 2 if the workload's preconditions fail (skip, not an error).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=bench/realworkload/lib.sh
source "$SCRIPT_DIR/lib.sh"

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <workload-spec.sh> <trace-out-dir>" >&2
  exit 1
fi

SPEC="$1"
TRACE_DIR="$2"
mkdir -p "$TRACE_DIR"

WORKLOAD_NAME=""
unset -f workload_preconditions workload_setup workload_cmd workload_teardown 2>/dev/null || true
# shellcheck source=/dev/null
source "$SPEC"

if [[ -z "$WORKLOAD_NAME" ]]; then
  echo "spec did not set WORKLOAD_NAME: $SPEC" >&2
  exit 1
fi

if declare -F workload_preconditions >/dev/null; then
  if ! workload_preconditions; then
    echo "skip $WORKLOAD_NAME: precondition failed" >&2
    exit 2
  fi
fi

lib="$(resolve_allocator tbjit)" || { echo "tbjit not built" >&2; exit 1; }

if declare -F workload_setup >/dev/null; then
  workload_setup
fi

CMD_STR="$(workload_cmd)"
trace_path="$TRACE_DIR/${WORKLOAD_NAME}.%p.trace"

# setarch -R: stable site ids (ASLR off). Degrade gracefully if unavailable.
SETARCH=()
if command -v setarch >/dev/null 2>&1; then
  SETARCH=(setarch -R)
fi

echo "[capture $WORKLOAD_NAME] $CMD_STR"
env_prefix="LD_PRELOAD=$lib TBJIT_TRACE_ONLY=1 TBJIT_TRACE=$trace_path "
rc=0
"${SETARCH[@]}" bash -c "${env_prefix}${CMD_STR}" || rc=$?
if [[ $rc -ne 0 ]]; then
  echo "[capture $WORKLOAD_NAME] workload exited $rc (trace may still be usable)" >&2
fi

if declare -F workload_teardown >/dev/null; then
  workload_teardown
fi

# Report what landed (largest file = dominant process).
shopt -s nullglob
traces=("$TRACE_DIR/${WORKLOAD_NAME}".*.trace)
shopt -u nullglob
if [[ ${#traces[@]} -eq 0 ]]; then
  echo "[capture $WORKLOAD_NAME] no trace written" >&2
  exit 1
fi
ls -lS "${traces[@]}" >&2
