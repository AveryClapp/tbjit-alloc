# Compiler-frontend workload (clang variant). Same fixture as
# gcc_compile so the picker dump for these two workloads can be
# differenced — interesting research-leverage signal: do gcc and
# clang allocate so differently that the picker installs different
# strategies for the same input?

# shellcheck source=bench/realworkload/workloads/_cxx_fixture.sh
source "$(dirname "${BASH_SOURCE[0]}")/_cxx_fixture.sh"

WORKLOAD_NAME="clang_compile"
WORKLOAD_DESC="clang++ -O2 -std=c++17 compiling the same C++17 fixture"

_CLANG_TMP=""

workload_preconditions() {
  command -v clang++ >/dev/null 2>&1 || { echo "clang++ missing" >&2; return 1; }
  return 0
}

workload_setup() {
  _CLANG_TMP=$(mktemp -d -t tbjit-clang-XXXXXX)
  export _CLANG_TMP
  generate_cxx_fixture "$_CLANG_TMP/fixture.cpp"
}

workload_cmd() {
  echo "clang++ -std=c++17 -O2 -c '$_CLANG_TMP/fixture.cpp' -o '$_CLANG_TMP/fixture.o'"
}

workload_teardown() {
  [[ -n "$_CLANG_TMP" && -d "$_CLANG_TMP" ]] && rm -rf "$_CLANG_TMP"
}
