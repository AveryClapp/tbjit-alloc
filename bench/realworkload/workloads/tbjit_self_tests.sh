# Meta-workload: run tbjit's own unit-test binaries (the bench layer's
# test/ directory) under LD_PRELOAD'd tbjit. Smallest possible sanity
# check that tbjit doesn't blow up when interposing its own
# infrastructure — and gives a baseline for "what does the picker see
# when fed tbjit-internal allocations." Useful as a smoke test in
# Phase 1 before pulling in slower real workloads.

WORKLOAD_NAME="tbjit_self_tests"
WORKLOAD_DESC="ctest --test-dir build --output-on-failure (tbjit's own suite)"

workload_preconditions() {
  command -v ctest >/dev/null 2>&1 || { echo "ctest missing" >&2; return 1; }
  [[ -d "$BUILD_DIR" ]] || { echo "build dir missing: $BUILD_DIR" >&2; return 1; }
  return 0
}

workload_cmd() {
  # --output-on-failure printed to /dev/null: we don't care about the
  # test report, only the allocation pattern of the test runner.
  echo "ctest --test-dir '$BUILD_DIR' --output-on-failure -j1 >/dev/null"
}
