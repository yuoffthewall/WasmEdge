#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Sweep driver — runs sightglass-cli over the non-SIMD suite for each
# WasmEdge mode and writes one JSON output per (mode) to OUT_DIR.
#
# Usage:
#   utils/run_sightglass_cli_sweep.sh [OUT_DIR]
#
# OUT_DIR defaults to /tmp/wasm-sg-sweep.
#
# Per-process sample count = PROCESSES * ITERATIONS_PER_PROCESS. Defaults
# (3 × 10 = 30 samples per kernel per phase) are conservative for paper
# measurements; override via SG_PROCESSES / SG_ITERS.
#
# Modes covered: tier-1 (IR_JIT alone), tier-2 + OSR (IR_JIT with the
# tier-2 LLVM background worker and OSR back-edge instrumentation
# enabled), and whole-module LLVM JIT.

set -euo pipefail

OUT_DIR="${1:-/tmp/wasm-sg-sweep}"
mkdir -p "$OUT_DIR"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# SIGHTGLASS_DIR is the upstream sightglass checkout. Default points at the
# location utils/setup_bench_deps.sh writes to. Override via the env var if
# you keep sightglass elsewhere.
SIGHTGLASS_DIR="${SIGHTGLASS_DIR:-$REPO_ROOT/bench/upstream/sightglass}"
export SIGHTGLASS_DIR

SG="${SG:-$SIGHTGLASS_DIR/target/release/sightglass-cli}"
ENGINE="${ENGINE:-$REPO_ROOT/build/tools/sightglass_engine/libwasmedge_engine.so}"
SUITE="${SUITE:-$REPO_ROOT/bench/wasmedge.suite}"
PROCESSES="${SG_PROCESSES:-3}"
ITERS="${SG_ITERS:-10}"
OPT_LEVEL="${WASMEDGE_IR_JIT_OPT_LEVEL:-2}"

# Plugin path for WASI-NN (used by image-classification kernel). Set when
# the plugin .so exists in the build tree; harmless when it doesn't.
PLUGIN_PATH="${WASMEDGE_PLUGIN_PATH:-$REPO_ROOT/build/plugins/wasi_nn}"
export WASMEDGE_PLUGIN_PATH="$PLUGIN_PATH"

# OpenVINO runtime libs must be on LD_LIBRARY_PATH for the WASI-NN plugin
# to dlopen them. Source setupvars.sh if it's present in the standard
# sightglass image-classification directory; this is a no-op for runs
# that don't include image-classification.
OPENVINO_VARS="${OPENVINO_VARS:-$SIGHTGLASS_DIR/benchmarks/image-classification/openvino/setupvars.sh}"
if [[ -f "$OPENVINO_VARS" ]]; then
  # shellcheck disable=SC1090
  source "$OPENVINO_VARS" >/dev/null 2>&1 || true
fi

if [[ ! -x "$SG" ]]; then
  echo "error: sightglass-cli not found at $SG" >&2
  echo "       run: $REPO_ROOT/utils/setup_bench_deps.sh" >&2
  exit 1
fi
if [[ ! -f "$ENGINE" ]]; then
  echo "error: engine library not found at $ENGINE" >&2
  echo "       build with: cd $REPO_ROOT/build && make wasmedge_engine -j32" >&2
  exit 1
fi
if [[ ! -f "$SUITE" ]]; then
  echo "error: suite file not found at $SUITE" >&2
  exit 1
fi

# Read kernel paths from suite (skip comments + blank lines), expanding
# ${SIGHTGLASS_DIR} and any other env-var references.
mapfile -t KERNELS < <(grep -vE '^\s*#|^\s*$' "$SUITE" | envsubst)

run_mode() {
  local mode="$1"; shift
  local label="$1"; shift
  local out="$OUT_DIR/$label.json"
  echo "=== mode=$label ($mode) → $out ==="
  WASMEDGE_SIGHTGLASS_MODE="$mode" \
  WASMEDGE_IR_JIT_OPT_LEVEL="$OPT_LEVEL" \
  WASMEDGE_QUIET=1 \
  "$@" \
  "$SG" benchmark \
    --engine "$ENGINE" \
    --processes "$PROCESSES" \
    --iterations-per-process "$ITERS" \
    --raw \
    --output-format json \
    --output-file "$out" \
    "${KERNELS[@]}"
}

# Tier-1 (no tier-2). Phase costs: compilation = parse + IR-JIT lowering at
# instantiate-time prep; instantiation = main IR-JIT pipeline.
run_mode IR_JIT tier1

# Tier-2 with OSR — IR JIT plus the LLVM background worker and OSR
# back-edge instrumentation. The thresholds match the in-tree harness
# (notes/design_docs/osr_doc.md §11).
run_mode IR_JIT tier2_osr \
  env WASMEDGE_TIER2_ENABLE=1 \
      WASMEDGE_TIER2_THRESHOLD=10 \
      WASMEDGE_OSR_THRESHOLD=5000

# Whole-module LLVM JIT — primary comparator.
run_mode JIT llvm_jit

# Interpreter omitted by default (multi-minute kernels). Uncomment if needed.
# run_mode Interpreter interp

echo
echo "=== sweep done ==="
echo "JSON outputs in $OUT_DIR"
echo
echo "Render a comparison table with:"
echo "  python3 $REPO_ROOT/utils/sightglass_json_table.py $OUT_DIR"
