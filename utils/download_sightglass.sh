#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2019-2024 Second State INC
#
# Download Sightglass benchmark .wasm kernels (non-SIMD / scalar workloads only)
# from bytecodealliance/sightglass into test/ir/testdata/sightglass/.
#
# Excludes SIMD-heavy benchmarks (see Sightglass benchmarks/simd.suite): libsodium,
# blake3-simd, hex-simd, intgemm-simd, meshoptimizer, tract-onnx (large ONNX asset),
# image-classification.

set -e

BASE_URL="https://github.com/bytecodealliance/sightglass/raw/main/benchmarks"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="${1:-$PROJECT_ROOT/test/ir/testdata/sightglass}"

mkdir -p "$OUT_DIR"
DOWNLOADED=0

download_one() {
  local dest="$1"
  local url="$2"
  if [ -f "$dest" ] && [ -s "$dest" ]; then
    echo "Skip (exists): $dest"
    return 0
  fi
  if command -v curl &>/dev/null; then
    if curl -fSL -o "$dest" "$url" 2>/dev/null; then
      echo "Downloaded: $dest"
      DOWNLOADED=$((DOWNLOADED + 1))
      return 0
    fi
  elif command -v wget &>/dev/null; then
    if wget -q -O "$dest" "$url" 2>/dev/null; then
      echo "Downloaded: $dest"
      DOWNLOADED=$((DOWNLOADED + 1))
      return 0
    fi
  else
    echo "Error: need curl or wget" >&2
    exit 1
  fi
  rm -f "$dest"
  echo "Skip (failed): $dest"
  return 1
}

# Download: local_stem|url — local file is OUT_DIR/<stem>.wasm
KERNELS="noop|${BASE_URL}/noop/benchmark.wasm
bz2|${BASE_URL}/bz2/benchmark.wasm
gcc-loops|${BASE_URL}/gcc-loops/benchmark.wasm
blake3-scalar|${BASE_URL}/blake3-scalar/benchmark.wasm
blind-sig|${BASE_URL}/blind-sig/benchmark.wasm
hashset|${BASE_URL}/hashset/benchmark.wasm
pulldown-cmark|${BASE_URL}/pulldown-cmark/benchmark.wasm
regex|${BASE_URL}/regex/benchmark.wasm
richards|${BASE_URL}/richards/benchmark.wasm
rust-compression|${BASE_URL}/rust-compression/benchmark.wasm
rust-html-rewriter|${BASE_URL}/rust-html-rewriter/benchmark.wasm
rust-json|${BASE_URL}/rust-json/benchmark.wasm
rust-protobuf|${BASE_URL}/rust-protobuf/benchmark.wasm
quicksort|${BASE_URL}/quicksort/benchmark.wasm
spidermonkey-json|${BASE_URL}/spidermonkey/spidermonkey-json.wasm
spidermonkey-markdown|${BASE_URL}/spidermonkey/spidermonkey-markdown.wasm
spidermonkey-regex|${BASE_URL}/spidermonkey/spidermonkey-regex.wasm
tinygo-json|${BASE_URL}/tinygo/tinygo-json.wasm
tinygo-regex|${BASE_URL}/tinygo/tinygo-regex.wasm
shootout-ackermann|${BASE_URL}/shootout/shootout-ackermann.wasm
shootout-base64|${BASE_URL}/shootout/shootout-base64.wasm
shootout-ctype|${BASE_URL}/shootout/shootout-ctype.wasm
shootout-ed25519|${BASE_URL}/shootout/shootout-ed25519.wasm
shootout-fib2|${BASE_URL}/shootout/shootout-fib2.wasm
shootout-gimli|${BASE_URL}/shootout/shootout-gimli.wasm
shootout-heapsort|${BASE_URL}/shootout/shootout-heapsort.wasm
shootout-keccak|${BASE_URL}/shootout/shootout-keccak.wasm
shootout-matrix|${BASE_URL}/shootout/shootout-matrix.wasm
shootout-memmove|${BASE_URL}/shootout/shootout-memmove.wasm
shootout-minicsv|${BASE_URL}/shootout/shootout-minicsv.wasm
shootout-nestedloop|${BASE_URL}/shootout/shootout-nestedloop.wasm
shootout-random|${BASE_URL}/shootout/shootout-random.wasm
shootout-ratelimit|${BASE_URL}/shootout/shootout-ratelimit.wasm
shootout-seqhash|${BASE_URL}/shootout/shootout-seqhash.wasm
shootout-sieve|${BASE_URL}/shootout/shootout-sieve.wasm
shootout-switch|${BASE_URL}/shootout/shootout-switch.wasm
shootout-xblabla20|${BASE_URL}/shootout/shootout-xblabla20.wasm
shootout-xchacha20|${BASE_URL}/shootout/shootout-xchacha20.wasm"

while IFS='|' read -r kernel url; do
  [ -z "$kernel" ] && continue
  download_one "$OUT_DIR/${kernel}.wasm" "$url" || true
done <<EOF
$KERNELS
EOF

# Kernel-prefixed inputs (mapped to default.input / default.input.md in the test harness)
INPUT_FILES="bz2.default.input|${BASE_URL}/bz2/default.input
blake3-scalar.default.input|${BASE_URL}/blake3-scalar/default.input
blake3-scalar.small.input|${BASE_URL}/blake3-scalar/small.input
blind-sig.secret.der|${BASE_URL}/blind-sig/secret.der
regex.default.input|${BASE_URL}/regex/default.input
rust-json.default.input|${BASE_URL}/rust-json/default.input
rust-protobuf.default.input|${BASE_URL}/rust-protobuf/default.input
rust-compression.default.input|${BASE_URL}/rust-compression/default.input
rust-html-rewriter.default.input|${BASE_URL}/rust-html-rewriter/default.input
pulldown-cmark.default.input.md|${BASE_URL}/pulldown-cmark/default.input.md"

while IFS='|' read -r outname url; do
  [ -z "$outname" ] && continue
  download_one "$OUT_DIR/$outname" "$url" || true
done <<EOF
$INPUT_FILES
EOF

# Shootout support files (inputs + expected stdout/stderr)
SHOOTOUT_URL="${BASE_URL}/shootout"
SUPPORT_FILES="shootout-ackermann.m.input
shootout-ackermann.n.input
shootout-ackermann.stdout.expected
shootout-ackermann.stderr.expected
shootout-base64.stdout.expected
shootout-base64.stderr.expected
shootout-ctype.stdout.expected
shootout-ctype.stderr.expected
shootout-ed25519.stdout.expected
shootout-ed25519.stderr.expected
shootout-fib2.stdout.expected
shootout-fib2.stderr.expected
shootout-gimli.stdout.expected
shootout-gimli.stderr.expected
shootout-heapsort.stdout.expected
shootout-heapsort.stderr.expected
shootout-keccak.stdout.expected
shootout-keccak.stderr.expected
shootout-matrix.stdout.expected
shootout-matrix.stderr.expected
shootout-memmove.stdout.expected
shootout-memmove.stderr.expected
shootout-minicsv.stdout.expected
shootout-minicsv.stderr.expected
shootout-nestedloop.stdout.expected
shootout-nestedloop.stderr.expected
shootout-random.stdout.expected
shootout-random.stderr.expected
shootout-ratelimit.stdout.expected
shootout-ratelimit.stderr.expected
shootout-seqhash.stdout.expected
shootout-seqhash.stderr.expected
shootout-sieve.stdout.expected
shootout-sieve.stderr.expected
shootout-switch.stdout.expected
shootout-switch.stderr.expected
shootout-xblabla20.stdout.expected
shootout-xblabla20.stderr.expected
shootout-xchacha20.stdout.expected
shootout-xchacha20.stderr.expected"

for sf in $SUPPORT_FILES; do
  dest="$OUT_DIR/$sf"
  [ -f "$dest" ] && continue
  url="${SHOOTOUT_URL}/${sf}"
  if command -v curl &>/dev/null; then
    if curl -fSL -o "$dest" "$url" 2>/dev/null; then
      echo "Downloaded: $sf"
      DOWNLOADED=$((DOWNLOADED + 1))
    else
      touch "$dest"
    fi
  elif command -v wget &>/dev/null; then
    if wget -q -O "$dest" "$url" 2>/dev/null; then
      echo "Downloaded: $sf"
      DOWNLOADED=$((DOWNLOADED + 1))
    else
      touch "$dest"
    fi
  fi
done

# Expected stdout/stderr: dest basename|upstream path under benchmarks/
EXPECTED_PAIRS="
noop.stdout.expected|noop/benchmark.stdout.expected
noop.stderr.expected|noop/benchmark.stderr.expected
bz2.stdout.expected|bz2/benchmark.stdout.expected
bz2.stderr.expected|bz2/benchmark.stderr.expected
gcc-loops.stdout.expected|gcc-loops/default.stdout.expected
blake3-scalar.stdout.expected|blake3-scalar/benchmark.stdout.expected
blake3-scalar.stderr.expected|blake3-scalar/benchmark.stderr.expected
blind-sig.stdout.expected|blind-sig/benchmark.stdout.expected
blind-sig.stderr.expected|blind-sig/benchmark.stderr.expected
regex.stdout.expected|regex/benchmark.stdout.expected
regex.stderr.expected|regex/benchmark.stderr.expected
rust-json.stdout.expected|rust-json/benchmark.stdout.expected
rust-json.stderr.expected|rust-json/benchmark.stderr.expected
rust-protobuf.stdout.expected|rust-protobuf/benchmark.stdout.expected
rust-compression.stdout.expected|rust-compression/benchmark.stdout.expected
rust-compression.stderr.expected|rust-compression/benchmark.stderr.expected
rust-html-rewriter.stdout.expected|rust-html-rewriter/benchmark.stdout.expected
rust-html-rewriter.stderr.expected|rust-html-rewriter/benchmark.stderr.expected
spidermonkey-json.stdout.expected|spidermonkey/spidermonkey-json.stdout.expected
spidermonkey-json.stderr.expected|spidermonkey/spidermonkey-json.stderr.expected
spidermonkey-markdown.stdout.expected|spidermonkey/spidermonkey-markdown.stdout.expected
spidermonkey-markdown.stderr.expected|spidermonkey/spidermonkey-markdown.stderr.expected
spidermonkey-regex.stdout.expected|spidermonkey/spidermonkey-regex.stdout.expected
spidermonkey-regex.stderr.expected|spidermonkey/spidermonkey-regex.stderr.expected
tinygo-json.stdout.expected|tinygo/tinygo-json.stdout.expected
tinygo-json.stderr.expected|tinygo/tinygo-json.stderr.expected
tinygo-regex.stdout.expected|tinygo/tinygo-regex.stdout.expected
tinygo-regex.stderr.expected|tinygo/tinygo-regex.stderr.expected"

while IFS='|' read -r outname relpath; do
  outname="$(echo "$outname" | tr -d '\r\n' | xargs)"
  relpath="$(echo "$relpath" | tr -d '\r\n' | xargs)"
  [ -z "$outname" ] && continue
  download_one "$OUT_DIR/$outname" "${BASE_URL}/${relpath}" || true
done <<EOF
$EXPECTED_PAIRS
EOF

# pulldown-cmark: legacy filenames + duplicate default.input.md
PULLDOWN_URL="${BASE_URL}/pulldown-cmark"
if [ ! -f "$OUT_DIR/default.input.md" ]; then
  curl -fSL -o "$OUT_DIR/default.input.md" "$PULLDOWN_URL/default.input.md" 2>/dev/null || wget -q -O "$OUT_DIR/default.input.md" "$PULLDOWN_URL/default.input.md" 2>/dev/null || touch "$OUT_DIR/default.input.md"
  echo "Downloaded: default.input.md"
  DOWNLOADED=$((DOWNLOADED + 1))
fi
download_one "$OUT_DIR/pulldown-cmark.stdout.expected" "$PULLDOWN_URL/benchmark.stdout.expected" || true
download_one "$OUT_DIR/pulldown-cmark.stderr.expected" "$PULLDOWN_URL/benchmark.stderr.expected" || true

# Optional: reject modules that require WebAssembly SIMD (v128)
if command -v wasm2wat &>/dev/null; then
  SIMD_FAIL=0
  while IFS= read -r -d '' f; do
    if ! wasm2wat --disable-simd "$f" &>/dev/null; then
      echo "warning: SIMD or invalid wasm (wasm2wat --disable-simd failed): $f" >&2
      SIMD_FAIL=1
    fi
  done < <(find "$OUT_DIR" -maxdepth 1 -name '*.wasm' -print0)
  if [ $SIMD_FAIL -eq 0 ]; then
    echo "All .wasm under $OUT_DIR pass non-SIMD check (wasm2wat --disable-simd)."
  fi
else
  echo "wasm2wat not found; skip SIMD validation."
fi

echo "Done. Downloaded or verified files in $OUT_DIR (increment counter approx: $DOWNLOADED)"
