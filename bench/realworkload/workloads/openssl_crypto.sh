# Cryptography workload: openssl speed (preinstalled, no apt). RSA/ECDSA
# keygen + sign/verify churns OpenSSL's BIGNUM allocator -- many short-lived,
# variable-size big-integer buffers plus per-op context allocations. A
# single-threaded, GC-free, mixed-size allocation pattern distinct from the
# compilers, scripting runtimes, jq, sqlite, and xz already in the corpus.

WORKLOAD_NAME="openssl_crypto"
WORKLOAD_DESC="openssl speed rsa2048/ecdsap256/sha256 (BIGNUM + buffer churn)"

workload_preconditions() {
  command -v openssl >/dev/null 2>&1 || { echo "openssl missing" >&2; return 1; }
  return 0
}

workload_cmd() {
  # -seconds bounds each primitive's run so total wall time is predictable.
  # rsa2048 and ecdsap256 are BIGNUM-heavy (sign/verify); sha256 adds a
  # streaming-buffer pattern. Single-threaded (no -multi) so no GC/thread
  # interaction.
  echo "openssl speed -seconds 2 rsa2048 ecdsap256 sha256 >/dev/null 2>&1"
}
