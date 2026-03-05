#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2019-2024 Second State INC
#
# Run Sightglass suite for each WASM kernel in testdata/sightglass in isolation.
# Interpreter and JIT modes; 15s timeout per run; abort/crash exits immediately.
#
# Usage:
#   run_sightglass_all.sh [BUILD_DIR]
#
# Arguments:
#   BUILD_DIR   Optional. Path to WasmEdge build directory (default: repo_root/build)
#
# Environment:
#   SIGHTGLASS_TIMEOUT     Per-run timeout in seconds (default: 15)
#   STOP_ON_FIRST_ABORT    If 1, exit after first abort/crash or failure (default: 0)
#
# Behavior:
#   - Runs every .wasm in test/ir/testdata/sightglass/ once in Interpreter and once in JIT.
#   - Each run is wrapped with timeout (15s). If the test crashes, core dumps, or errors,
#     it exits immediately; only hung runs wait for the full timeout.
#   - Ctrl+C (SIGINT) or kill (SIGTERM) exits the script cleanly.
#
# Requirements:
#   - Build with -DWASMEDGE_BUILD_TESTS=ON -DWASMEDGE_BUILD_IR_JIT=ON -DWASMEDGE_USE_LLVM=ON
#   - Sightglass testdata: run utils/download_sightglass.sh if testdata/sightglass is missing
#
# Examples:
#   ./run_sightglass_all.sh
#   ./run_sightglass_all.sh /path/to/wasmedge/build
#   SIGHTGLASS_TIMEOUT=30 ./run_sightglass_all.sh
#   STOP_ON_FIRST_ABORT=1 ./run_sightglass_all.sh

set -e
TIMEOUT_SEC="${SIGHTGLASS_TIMEOUT:-15}"
STOP_ON_FIRST_ABORT="${STOP_ON_FIRST_ABORT:-0}"

# Ctrl+C / SIGTERM: exit script immediately (not just the current child)
trap 'echo ""; echo "Interrupted (Ctrl+C). Exiting."; exit 130' INT
trap 'echo ""; echo "Terminated. Exiting."; exit 143' TERM

# --help
for arg in "$@"; do
  case "$arg" in
    -h|--help)
      echo "Usage:"
      echo "  run_sightglass_all.sh [BUILD_DIR]"
      echo ""
      echo "Arguments:"
      echo "  BUILD_DIR   Optional. Path to WasmEdge build directory (default: repo_root/build)"
      echo ""
      echo "Environment:"
      echo "  SIGHTGLASS_TIMEOUT     Per-run timeout in seconds (default: 15)"
      echo "  STOP_ON_FIRST_ABORT    If 1, exit after first abort/crash or failure (default: 0)"
      echo ""
      echo "Behavior:"
      echo "  - Runs every .wasm in test/ir/testdata/sightglass/ in Interpreter and JIT."
      echo "  - Timeout 15s per run; crash/core-dump/error exits immediately."
      echo "  - Ctrl+C exits the script."
      echo ""
      echo "Examples:"
      echo "  ./run_sightglass_all.sh"
      echo "  ./run_sightglass_all.sh /path/to/wasmedge/build"
      echo "  SIGHTGLASS_TIMEOUT=30 ./run_sightglass_all.sh"
      echo "  STOP_ON_FIRST_ABORT=1 ./run_sightglass_all.sh"
      exit 0
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WASMEDGE_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SIGHTGLASS_DIR="$SCRIPT_DIR/testdata/sightglass"
BUILD_DIR="${1:-$WASMEDGE_ROOT/build}"
TEST_BIN="$BUILD_DIR/test/ir/wasmedgeIRBenchmarkTests"

if [ ! -d "$SIGHTGLASS_DIR" ]; then
  echo "Sightglass testdata not found: $SIGHTGLASS_DIR"
  echo "Run: bash $WASMEDGE_ROOT/utils/download_sightglass.sh"
  exit 1
fi

if [ ! -x "$TEST_BIN" ]; then
  echo "Test binary not found or not executable: $TEST_BIN"
  echo "Build with: cmake -DWASMEDGE_BUILD_TESTS=ON -DWASMEDGE_BUILD_IR_JIT=ON -DWASMEDGE_USE_LLVM=ON ..."
  exit 1
fi

# All .wasm kernels in testdata/sightglass (sorted)
KERNELS=()
while IFS= read -r -d '' p; do
  KERNELS+=("$(basename "$p" .wasm)")
done < <(find "$SIGHTGLASS_DIR" -maxdepth 1 -name "*.wasm" -print0 | sort -z)
[ ${#KERNELS[@]} -eq 0 ] && echo "No .wasm in $SIGHTGLASS_DIR" && exit 1

echo "=== Sightglass suite: run each kernel in isolation (Interpreter + JIT) ==="
echo "Test binary: $TEST_BIN"
echo "Timeout: ${TIMEOUT_SEC}s per run (crash/error exits immediately)"
echo "Kernels: ${KERNELS[*]}"
echo ""

cd "$WASMEDGE_ROOT"

PASS_INTERP=()
FAIL_INTERP=()
ABORT_INTERP=()
PASS_JIT=()
FAIL_JIT=()
ABORT_JIT=()

for k in "${KERNELS[@]}"; do
  for mode in Interpreter JIT; do
    export WASMEDGE_SIGHTGLASS_KERNEL="$k"
    export WASMEDGE_SIGHTGLASS_MODE="$mode"
    out=$(mktemp)
    retfile=$(mktemp)
    set +e
    start=$(date +%s.%N)
    # Run in new session (setsid) so abort/signal from test does not hit this shell; run still ends
    # immediately when the test exits (timeout returns as soon as child exits).
    if command -v setsid >/dev/null 2>&1; then
      setsid sh -c "timeout \"$TIMEOUT_SEC\" \"$TEST_BIN\" --gtest_filter='*SightglassSuite*' >\"$out\" 2>&1; echo \$? >\"$retfile\"" </dev/null
    else
      ( timeout "$TIMEOUT_SEC" "$TEST_BIN" --gtest_filter='*SightglassSuite*' >"$out" 2>&1; r=$?; printf '%s' "$r" >"$retfile"; exit 0 )
    fi
    ret=$(cat "$retfile")
    end=$(date +%s.%N)
    set -e
    rm -f "$retfile"
    dur=$(echo "$end - $start" | bc 2>/dev/null || echo "?")
    if [ $ret -eq 0 ]; then
      if grep -q "Inst.Lat\|WorkTime\|TtV" "$out" 2>/dev/null; then
        [ "$mode" = "Interpreter" ] && PASS_INTERP+=("$k") || PASS_JIT+=("$k")
      else
        [ "$mode" = "Interpreter" ] && FAIL_INTERP+=("$k") || FAIL_JIT+=("$k")
        echo "  $k $mode: ${dur}s (fail)"
      fi
    elif [ $ret -eq 124 ]; then
      [ "$mode" = "Interpreter" ] && ABORT_INTERP+=("$k (timeout)") || ABORT_JIT+=("$k (timeout)")
      echo "  $k $mode: ${dur}s (timeout)"
    elif [ $ret -ge 128 ] && [ $ret -le 165 ]; then
      [ "$mode" = "Interpreter" ] && ABORT_INTERP+=("$k") || ABORT_JIT+=("$k")
      echo "  $k $mode: ${dur}s (abort/crash)"
      [ "$STOP_ON_FIRST_ABORT" = "1" ] && rm -f "$out" && break 2
    else
      [ "$mode" = "Interpreter" ] && FAIL_INTERP+=("$k") || FAIL_JIT+=("$k")
      echo "  $k $mode: ${dur}s (exit $ret)"
      [ "$STOP_ON_FIRST_ABORT" = "1" ] && rm -f "$out" && break 2
    fi
    rm -f "$out"
  done
  unset WASMEDGE_SIGHTGLASS_KERNEL WASMEDGE_SIGHTGLASS_MODE
done

echo "========== RESULTS =========="
echo ""
echo "Interpreter mode:"
echo "  PASS (${#PASS_INTERP[@]}): ${PASS_INTERP[*]:-none}"
echo "  FAIL (${#FAIL_INTERP[@]}): ${FAIL_INTERP[*]:-none}"
echo "  ABORT/CRASH (${#ABORT_INTERP[@]}): ${ABORT_INTERP[*]:-none}"
echo ""
echo "JIT mode:"
echo "  PASS (${#PASS_JIT[@]}): ${PASS_JIT[*]:-none}"
echo "  FAIL (${#FAIL_JIT[@]}): ${FAIL_JIT[*]:-none}"
echo "  ABORT/CRASH (${#ABORT_JIT[@]}): ${ABORT_JIT[*]:-none}"
echo ""
echo "Summary: ${#PASS_INTERP[@]}/${#KERNELS[@]} Interpreter OK, ${#PASS_JIT[@]}/${#KERNELS[@]} JIT OK"
