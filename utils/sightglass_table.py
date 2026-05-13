#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Emit a Markdown WorkTime comparison table from the 9 per-run logs that
# `run_sightglass_sweep.sh` produces.  Rows sorted by LLVM/t2 descending;
# `noop` placed last with "—" ratios.  Geomean aggregates computed on the
# 32 non-`noop` kernels.
#
# Usage:
#   python3 utils/sightglass_table.py [LOG_PREFIX]
#
# Default LOG_PREFIX: /tmp/wasm-sweep   (must match the sweep script).
# Reads  <LOG_PREFIX>-{tier1,tier2,llvm}-run{1,2,3}.log
# Writes the Markdown table to stdout.

import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

# Matches the harness's per-kernel output line:
#   "<kernel>  <MODE>  <InstLat>  <WorkTime>  <TtV>"
KERNEL_RE = re.compile(
    r"^(\S+)\s+(IR_JIT|JIT|AOT|Interpreter)\s+"
    r"([0-9]+(?:\.[0-9]+)?)\s+"
    r"([0-9]+(?:\.[0-9]+)?)\s+"
    r"([0-9]+(?:\.[0-9]+)?)\s*$"
)


def parse_log(path: Path) -> dict:
    out = {}
    with open(path) as fh:
        for line in fh:
            m = KERNEL_RE.match(line)
            if m:
                kernel, _mode, il, wt, ttv = m.groups()
                out[kernel] = (float(il), float(wt), float(ttv))
    return out


def collect(prefix: str) -> dict:
    d = defaultdict(lambda: defaultdict(list))
    for mode in ("tier1", "tier2", "llvm"):
        for run in (1, 2, 3):
            p = Path(f"{prefix}-{mode}-run{run}.log")
            if p.exists():
                for k, v in parse_log(p).items():
                    d[mode][k].append(v)
    med = defaultdict(dict)
    for mode in d:
        for k, runs in d[mode].items():
            med[mode][k] = statistics.median([r[1] for r in runs])  # WT only
    return med


def geomean(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else None


def main() -> int:
    prefix = sys.argv[1] if len(sys.argv) > 1 else "/tmp/wasm-sweep"
    med = collect(prefix)
    if not med.get("tier1"):
        print(f"error: no tier1 logs found at {prefix}-tier1-run*.log",
              file=sys.stderr)
        return 1

    kernels = sorted(med["tier1"].keys())
    rows = []
    for k in kernels:
        t1 = med["tier1"].get(k)
        t2 = med["tier2"].get(k)
        lv = med["llvm"].get(k)
        # noop has sub-µs WT; ratios are meaningless.
        if k == "noop":
            rows.append((k, t1, t2, lv, None, None))
            continue
        r1 = t1 / t2 if t1 and t2 and t2 > 0 else None
        r2 = lv / t2 if lv and t2 and t2 > 0 else None
        rows.append((k, t1, t2, lv, r1, r2))

    # Sort by LLVM/t2 descending; noop (None) at the bottom.
    rows.sort(key=lambda r: (r[5] is None, -(r[5] or 0)))

    fmt = lambda v: f"{v:,.0f}" if v is not None else "—"
    rat = lambda v: f"{v:.2f}×" if v is not None else "—"

    print("| Kernel | Tier-1 | Tier-2 | LLVM JIT | t1/t2 | LLVM/t2 |")
    print("|---|---:|---:|---:|---:|---:|")
    for k, t1, t2, lv, r1, r2 in rows:
        print(f"| {k} | {fmt(t1)} | {fmt(t2)} | {fmt(lv)} | "
              f"{rat(r1)} | {rat(r2)} |")

    kernels_agg = [k for k in kernels if k != "noop"]
    t1_over_t2 = [med["tier1"][k] / med["tier2"][k]
                  for k in kernels_agg
                  if med["tier1"].get(k) and med["tier2"].get(k)
                  and med["tier2"][k] > 0]
    lv_over_t2 = [med["llvm"][k] / med["tier2"][k]
                  for k in kernels_agg
                  if med["llvm"].get(k) and med["tier2"].get(k)
                  and med["tier2"][k] > 0]
    print()
    print(f"Aggregates ({len(kernels_agg)} kernels, `noop` excluded): "
          f"geomean t1/t2 **{geomean(t1_over_t2):.3f}×**, "
          f"geomean LLVM/t2 **{geomean(lv_over_t2):.3f}×**.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
