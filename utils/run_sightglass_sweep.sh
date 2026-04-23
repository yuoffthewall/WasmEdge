#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run a 3-run × 3-mode sightglass-strong sweep (tier-1, tier-2+OSR, LLVM JIT)
# and write per-run logs suitable for `sightglass_table.py`.
#
# Must be invoked from the cmake build directory. Expects
# `./test/ir/wasmedgeIRBenchmarkTests` (built Release for perf) and
# `../test/ir/testdata/sightglass-strong/*.wasm` to exist.
#
# Usage:
#   cd build
#   ../utils/run_sightglass_sweep.sh [LOG_PREFIX]
#
# Default LOG_PREFIX: /tmp/wasm-sweep
# Writes /tmp/wasm-sweep-{tier1,tier2,llvm}-run{1,2,3}.log (9 files).

set -u
LOG_PREFIX="${1:-/tmp/wasm-sweep}"
SUITE_DIR="../test/ir/testdata/sightglass-strong"
BINARY="./test/ir/wasmedgeIRBenchmarkTests"

if [[ ! -x "$BINARY" ]]; then
  echo "error: $BINARY not found — run from the cmake build directory" >&2
  exit 1
fi
if [[ ! -d "$SUITE_DIR" ]]; then
  echo "error: suite dir $SUITE_DIR missing" >&2
  exit 1
fi

run_sweep() {
  local mode="$1"; shift
  local run="$1"; shift
  local logfile="${LOG_PREFIX}-${mode}-run${run}.log"
  echo "=== mode=${mode} run=${run} start $(date -Iseconds) ===" | tee "${logfile}"
  for wasm in "${SUITE_DIR}"/*.wasm; do
    kernel="$(basename "$wasm" .wasm)"
    echo "Testing $kernel:" | tee -a "${logfile}"
    "$@" \
      WASMEDGE_SIGHTGLASS_DIR=sightglass-strong \
      WASMEDGE_SIGHTGLASS_KERNEL="$kernel" \
      WASMEDGE_IR_JIT_OPT_LEVEL=2 \
      WASMEDGE_QUIET=1 \
      stdbuf -oL timeout 120 "$BINARY" --gtest_filter='*SightglassSuite*' \
        >> "${logfile}" 2>&1
  done
  echo "=== mode=${mode} run=${run} end $(date -Iseconds) ===" | tee -a "${logfile}"
}

for run in 1 2 3; do
  # tier-1 arm: IR_JIT with tier-2 disabled.
  run_sweep tier1 "${run}" env WASMEDGE_SIGHTGLASS_MODE=IR_JIT

  # tier-2 + OSR arm.
  run_sweep tier2 "${run}" env \
    WASMEDGE_SIGHTGLASS_MODE=IR_JIT \
    WASMEDGE_TIER2_ENABLE=1 \
    WASMEDGE_TIER2_THRESHOLD=10 \
    WASMEDGE_OSR_THRESHOLD=5000

  # LLVM JIT arm (whole-module).
  run_sweep llvm "${run}" env WASMEDGE_SIGHTGLASS_MODE=JIT
done

echo "=== all sweeps done $(date -Iseconds) ==="
echo
echo "Verify with:"
echo "  for f in ${LOG_PREFIX}-*.log; do grep -c PASSED \"\$f\"; done"
echo "  grep -iE 'dumped|error|failed|mismatch|warning' ${LOG_PREFIX}-*.log"
echo
echo "Render the comparison table with:"
echo "  python3 ../utils/sightglass_table.py ${LOG_PREFIX}"
