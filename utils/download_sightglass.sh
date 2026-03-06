#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2019-2024 Second State INC
#
# Download Sightglass benchmark .wasm kernels from bytecodealliance/sightglass
# into test/ir/testdata/sightglass/.

set -e

BASE_URL="https://github.com/bytecodealliance/sightglass/raw/main/benchmarks"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="${1:-$PROJECT_ROOT/test/ir/testdata/sightglass}"

mkdir -p "$OUT_DIR"
DOWNLOADED=0

# Download: output_name|url_path (one per line)
# Standalone benchmarks use benchmark.wasm; shootout uses shootout-<name>.wasm
KERNELS="pulldown-cmark|${BASE_URL}/pulldown-cmark/benchmark.wasm
quicksort|${BASE_URL}/quicksort/benchmark.wasm
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
  dest="$OUT_DIR/${kernel}.wasm"
  if [ -f "$dest" ] && [ -s "$dest" ]; then
    echo "Skip (exists): $dest"
    continue
  fi
  if command -v curl &>/dev/null; then
    if curl -fSL -o "$dest" "$url" 2>/dev/null; then
      echo "Downloaded: $kernel -> $dest"
      DOWNLOADED=$((DOWNLOADED + 1))
    else
      echo "Skip (failed): $kernel"
      rm -f "$dest"
    fi
  elif command -v wget &>/dev/null; then
    if wget -q -O "$dest" "$url" 2>/dev/null; then
      echo "Downloaded: $kernel -> $dest"
      DOWNLOADED=$((DOWNLOADED + 1))
    else
      echo "Skip (failed): $kernel"
      rm -f "$dest"
    fi
  else
    echo "Error: need curl or wget" >&2
    exit 1
  fi
done <<EOF
$KERNELS
EOF

# Download supporting files (.input, .stdout.expected, .stderr.expected) for shootout
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
  if [ -f "$dest" ]; then
    continue
  fi
  url="${SHOOTOUT_URL}/${sf}"
  if command -v curl &>/dev/null; then
    if curl -fSL -o "$dest" "$url" 2>/dev/null; then
      echo "Downloaded: $sf"
      DOWNLOADED=$((DOWNLOADED + 1))
    else
      # Empty expected files may 404; create an empty file
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

echo "Done. Downloaded $DOWNLOADED file(s) to $OUT_DIR"
