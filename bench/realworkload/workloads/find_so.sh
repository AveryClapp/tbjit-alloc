# Streaming filesystem scan. The smallest "real" workload — exercises
# libc's malloc on getdents + path string copies. Quick to run and a
# decent stress-test for the picker's ability to identify monomorphic
# tight-loop sites without paying for the full Phase-1 toolchain
# (compilers, databases) being installed.

WORKLOAD_NAME="find_so"
WORKLOAD_DESC="find /usr -type f -name '*.so' (streaming directory traversal)"

workload_preconditions() {
  command -v find >/dev/null 2>&1 || { echo "find missing" >&2; return 1; }
  [[ -d /usr ]] || { echo "/usr missing" >&2; return 1; }
  return 0
}

workload_cmd() {
  # Redirect output to /dev/null — we measure allocation behavior, not
  # the cost of writing matches to a pipe.
  echo "find /usr -type f -name '*.so' >/dev/null"
}
