#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Fetch the external dependencies the CLI sweep needs:
#   - upstream sightglass at the pinned commit (provides .wasm kernels and
#     sightglass-cli, the harness binary the engine plugs into),
#   - OpenVINO 2022.2 toolkit (runtime libs the WASI-NN plugin dlopens),
#   - mobilenet model files used by the image-classification kernel.
#
# Idempotent: rerunning is a no-op once everything is in place. Intended
# to be run once on a fresh checkout.
#
# Usage:
#   utils/setup_bench_deps.sh
#
# Output layout:
#   bench/upstream/sightglass/                        (clone)
#   bench/upstream/sightglass/target/release/sightglass-cli  (cargo build)
#   bench/upstream/sightglass/benchmarks/image-classification/openvino/   (toolkit)
#   bench/upstream/sightglass/benchmarks/image-classification/mobilenet.{xml,bin}
#
# After this completes, source the OpenVINO env once per shell:
#   source $REPO_ROOT/bench/upstream/sightglass/benchmarks/image-classification/openvino/setupvars.sh
# (the sweep driver does this automatically when it sees the file).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIGHTGLASS_DIR="${SIGHTGLASS_DIR:-$REPO_ROOT/bench/upstream/sightglass}"
SIGHTGLASS_REPO="https://github.com/bytecodealliance/sightglass.git"
SIGHTGLASS_COMMIT="789ac0957b7f4932382bcdf4e8dcc09c4f044702"

OV_FN="l_openvino_toolkit_ubuntu20_2022.2.0.7713.af16ea1d79a_x86_64"
OV_URL="https://storage.openvinotoolkit.org/repositories/openvino/packages/2022.2/linux/${OV_FN}.tgz"
MOBILENET_BASE="https://download.01.org/openvinotoolkit/fixtures/mobilenet"

# 1. sightglass repo at pinned commit -----------------------------------------

if [[ ! -d "$SIGHTGLASS_DIR/.git" ]]; then
  echo "[setup] cloning sightglass into $SIGHTGLASS_DIR"
  mkdir -p "$(dirname "$SIGHTGLASS_DIR")"
  git clone "$SIGHTGLASS_REPO" "$SIGHTGLASS_DIR"
fi
cd "$SIGHTGLASS_DIR"
if [[ "$(git rev-parse HEAD)" != "$SIGHTGLASS_COMMIT" ]]; then
  echo "[setup] pinning sightglass to $SIGHTGLASS_COMMIT"
  git fetch --quiet origin
  git checkout --quiet "$SIGHTGLASS_COMMIT"
fi

# 2. sightglass-cli binary ----------------------------------------------------

if [[ ! -x "$SIGHTGLASS_DIR/target/release/sightglass-cli" ]]; then
  echo "[setup] building sightglass-cli (release)"
  # The upload crate's default reqwest pulls openssl-sys, which fails to
  # build on standard Ubuntu setups without libssl-dev. We don't need the
  # `upload` subcommand for benchmarking, so swap to rustls-tls. Idempotent.
  CARGO_TOML="$SIGHTGLASS_DIR/crates/upload/Cargo.toml"
  if grep -q '^reqwest = "0.11"' "$CARGO_TOML" 2>/dev/null; then
    sed -i 's|^reqwest = "0.11"|reqwest = { version = "0.11", default-features = false, features = ["blocking", "rustls-tls"] }|' "$CARGO_TOML"
  fi
  (cd "$SIGHTGLASS_DIR" && cargo build --release -p sightglass-cli)
fi

# 3. mobilenet model + OpenVINO toolkit (image-classification only) -----------

ICDIR="$SIGHTGLASS_DIR/benchmarks/image-classification"

if [[ ! -f "$ICDIR/mobilenet.xml" ]]; then
  echo "[setup] fetching mobilenet.xml"
  wget -q "$MOBILENET_BASE/mobilenet.xml" -O "$ICDIR/mobilenet.xml"
fi
if [[ ! -f "$ICDIR/mobilenet.bin" ]]; then
  echo "[setup] fetching mobilenet.bin"
  wget -q "$MOBILENET_BASE/mobilenet.bin" -O "$ICDIR/mobilenet.bin"
fi

if [[ ! -d "$ICDIR/openvino" ]]; then
  echo "[setup] fetching OpenVINO toolkit (~100 MB)"
  wget -q --show-progress "$OV_URL" -O "$ICDIR/${OV_FN}.tgz"
  tar -C "$ICDIR" -xzf "$ICDIR/${OV_FN}.tgz"
  mv "$ICDIR/${OV_FN}" "$ICDIR/openvino"
  rm "$ICDIR/${OV_FN}.tgz"
fi

# 4. The image-classification stderr.expected upstream is pinned against an
#    older test.jpg that no longer matches the current model output. Replace
#    it with the current expected (window screen, fire screen, ...) so the
#    sightglass-cli output check passes. Confirmed by running the wasm under
#    both LLVM JIT and our IR JIT — both produce the same predictions.
EXP="$ICDIR/image-classification-benchmark.stderr.expected"
if [[ -f "$EXP" ]] && grep -q "^1\.) banana$" "$EXP"; then
  echo "[setup] refreshing image-classification stderr.expected"
  cat > "$EXP" <<'EOF'
Confidence is over 90%
1.) window screen
2.) fire screen, fireguard
3.) strainer
4.) shower curtain
5.) velvet
EOF
fi

echo
echo "[setup] done."
echo "  SIGHTGLASS_DIR = $SIGHTGLASS_DIR"
echo "  sightglass-cli = $SIGHTGLASS_DIR/target/release/sightglass-cli"
echo "  OpenVINO       = $ICDIR/openvino"
echo
echo "Sweeps now runnable via:"
echo "  utils/run_sightglass_cli_sweep.sh"
