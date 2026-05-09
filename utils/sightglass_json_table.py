#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Reads sightglass-cli `--raw --output-format json` files (one per mode)
# and emits a per-phase comparison table in markdown.
#
# Usage:
#   python3 utils/sightglass_json_table.py [DIR]
#
# DIR defaults to /tmp/wasm-sg-sweep. The script looks for `*.json` files
# in DIR and treats each one as a "mode" labelled by its filename stem
# (e.g., tier1.json → "tier1", llvm_jit.json → "llvm_jit").
#
# For each (kernel, phase), the median across all (process, iteration)
# samples is reported. Phases: Compilation, Instantiation, Execution,
# plus a derived TtV = sum of the three medians.

import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

PHASES = ("Compilation", "Instantiation", "Execution")


def load(path: Path) -> dict:
    """Returns {kernel_basename: {phase: [count, ...]}}."""
    out: dict = defaultdict(lambda: defaultdict(list))
    with open(path) as fh:
        records = json.load(fh)
    for r in records:
        # Per-kernel-dir layout (.../sightglass-strong/<kernel>/benchmark.wasm)
        # produces basename "benchmark" for every kernel — fall back to the
        # parent dir name. Old flat layout (<kernel>.wasm) is also accepted.
        wasm_path = Path(r["wasm"])
        kernel = wasm_path.parent.name if wasm_path.stem == "benchmark" else wasm_path.stem
        out[kernel][r["phase"]].append(int(r["count"]))
    return out


def medians(samples: dict) -> dict:
    """For each kernel/phase, take the median across (process, iter)."""
    return {
        kernel: {
            phase: statistics.median(values) if values else None
            for phase, values in phases.items()
        }
        for kernel, phases in samples.items()
    }


def geomean(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else None


def fmt_count(v):
    return f"{v:,.0f}" if v is not None else "—"


def fmt_ratio(v):
    return f"{v:.2f}×" if v is not None else "—"


def render(modes: dict, baseline: str = "llvm_jit") -> None:
    """modes: {mode_label: {kernel: {phase: median_count}}}.
    Tables: per-kernel rows; columns are mode × phase, plus phase
    speedup vs baseline.
    """
    # Set of all kernels present in any mode
    kernels = sorted(set(k for m in modes.values() for k in m.keys()))
    mode_labels = sorted(modes.keys())

    if baseline not in modes and modes:
        baseline = next(iter(modes))

    # ---- Table 1: per-phase counts (cycles) for every mode ----
    print(f"# Sightglass — phase breakdown (median {modes_unit_hint(modes)} per kernel)")
    print()
    non_baseline = [m for m in mode_labels if m != baseline]

    if baseline:
        print(f"`{baseline}/<mode>` columns are speedup ratios — "
              f"`{baseline} count / <mode> count`. > 1 means `<mode>` is "
              f"faster than `{baseline}` at that phase.")
        print()
    cols = ["Kernel"]
    for mode in mode_labels:
        for ph in PHASES:
            cols.append(f"{mode} {ph[:5]}")
    if baseline:
        for mode in non_baseline:
            for ph in PHASES:
                cols.append(f"{baseline}/{mode} {ph[:5]}")
    print("| " + " | ".join(cols) + " |")
    sep = ["---"] + ["---:"] * (len(cols) - 1)
    print("| " + " | ".join(sep) + " |")

    for k in kernels:
        row = [k]
        for mode in mode_labels:
            for ph in PHASES:
                row.append(fmt_count(modes[mode].get(k, {}).get(ph)))
        if baseline:
            base = modes.get(baseline, {}).get(k, {})
            for mode in non_baseline:
                for ph in PHASES:
                    base_v = base.get(ph)
                    this_v = modes[mode].get(k, {}).get(ph)
                    if not base_v or not this_v or this_v <= 0:
                        row.append("—")
                    else:
                        row.append(fmt_ratio(base_v / this_v))
        print("| " + " | ".join(row) + " |")

    # ---- Aggregates ----
    print()
    print("**Geomean speedup of non-baseline modes vs baseline**")
    print()
    print("| Mode | " + " | ".join(PHASES) + " | TtV |")
    print("|---|" + "---:|" * (len(PHASES) + 1))
    base_data = modes.get(baseline, {})
    for mode in mode_labels:
        if mode == baseline:
            continue
        ratios = {ph: [] for ph in PHASES}
        ttv_ratios = []
        for k in kernels:
            base_phases = base_data.get(k, {})
            this_phases = modes[mode].get(k, {})
            for ph in PHASES:
                if base_phases.get(ph) and this_phases.get(ph) and this_phases[ph] > 0:
                    ratios[ph].append(base_phases[ph] / this_phases[ph])
            base_ttv = sum(base_phases.get(ph, 0) for ph in PHASES) or None
            this_ttv = sum(this_phases.get(ph, 0) for ph in PHASES) or None
            if base_ttv and this_ttv and this_ttv > 0:
                ttv_ratios.append(base_ttv / this_ttv)
        cells = [mode] + [fmt_ratio(geomean(ratios[ph])) for ph in PHASES] + [
            fmt_ratio(geomean(ttv_ratios))]
        print("| " + " | ".join(cells) + " |")


def modes_unit_hint(modes: dict) -> str:
    """sightglass default measure is `cycles`. We display that label here."""
    return "cycles"


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("/tmp/wasm-sg-sweep")
    if not src.is_dir():
        print(f"error: not a directory: {src}", file=sys.stderr)
        return 1
    modes = {}
    for json_file in sorted(src.glob("*.json")):
        label = json_file.stem
        modes[label] = medians(load(json_file))
    if not modes:
        print(f"error: no .json files in {src}", file=sys.stderr)
        return 1
    render(modes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
