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
base64|${BASE_URL}/shootout/shootout-base64.wasm
ed25519|${BASE_URL}/shootout/shootout-ed25519.wasm
fib|${BASE_URL}/shootout/shootout-fib2.wasm
quicksort|${BASE_URL}/quicksort/benchmark.wasm"

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

echo "Done. Downloaded $DOWNLOADED file(s) to $OUT_DIR"
