# Developer-shell workload: `git log` walking the tbjit repo itself.
# Exercises object-database reads, commit-graph traversal, and the
# small-string churn typical of git's plumbing. Self-contained: the
# repo is whatever directory CI checks out, so no external fixture is
# needed.

WORKLOAD_NAME="git_log"
WORKLOAD_DESC="git log --no-color --oneline -n 5000 (object-db walk)"

workload_preconditions() {
  command -v git >/dev/null 2>&1 || { echo "git missing" >&2; return 1; }
  # Must be inside a git repo. The runner invokes us from REPO_ROOT.
  git -C "$REPO_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || { echo "not a git repo: $REPO_ROOT" >&2; return 1; }
  return 0
}

workload_cmd() {
  # -n 5000 caps work so the run is bounded even on a deep history clone.
  # --no-pager / >/dev/null prevent the pager from blocking under LD_PRELOAD.
  echo "git -C '$REPO_ROOT' --no-pager log --no-color --oneline -n 5000 >/dev/null"
}
