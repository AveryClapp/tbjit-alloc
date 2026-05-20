# Compiler-frontend workload: g++ compiling a moderately heavy C++17
# file. Fixture comes from _cxx_fixture.sh (shared with clang_compile)
# so manifest deltas reflect compiler-side differences rather than
# input drift. Template-heavy + STL containers gives the polymorphic-
# size churn canonical for compiler workloads.

# shellcheck source=bench/realworkload/workloads/_cxx_fixture.sh
source "$(dirname "${BASH_SOURCE[0]}")/_cxx_fixture.sh"

WORKLOAD_NAME="gcc_compile"
WORKLOAD_DESC="g++ -O2 -std=c++17 compiling a template-heavy fixture (parse + codegen)"

_GCC_TMP=""

workload_preconditions() {
  command -v g++ >/dev/null 2>&1 || { echo "g++ missing" >&2; return 1; }
  return 0
}

workload_setup() {
  _GCC_TMP=$(mktemp -d -t tbjit-gcc-XXXXXX)
  export _GCC_TMP
  generate_cxx_fixture "$_GCC_TMP/fixture.cpp"
}

workload_cmd() {
  # -c so we don't link; -O2 to actually run the middle/back-end.
  echo "g++ -std=c++17 -O2 -c '$_GCC_TMP/fixture.cpp' -o '$_GCC_TMP/fixture.o'"
}

workload_teardown() {
  [[ -n "$_GCC_TMP" && -d "$_GCC_TMP" ]] && rm -rf "$_GCC_TMP"
}
