"""Optional external deps for vvizard test entrypoints.

Missing optional tools/trees → callers print SKIP and exit 0.
Present tools that then fail → hard FAIL (nonzero). Missing vvizard binary
is a build responsibility (FAIL), not SKIP.
"""
from __future__ import annotations

import os
import shutil
from pathlib import Path

_which = shutil.which

OPTIONAL_SIM_TOOLS = ("iverilog", "vvp")
OPTIONAL_MIPS_TOOLS = ("mips-linux-gnu-as", "mips-linux-gnu-objcopy")
OPTIONAL_RTL_TOOLS = OPTIONAL_SIM_TOOLS + OPTIONAL_MIPS_TOOLS


def missing_tools(names: tuple[str, ...] | list[str], which=None) -> list[str]:
    which = which or _which
    return [n for n in names if which(n) is None]


def cs150_gaps(root: Path) -> list[str]:
    gaps: list[str] = []
    for rel in (
        "hardware/sim-icarus/tb/CoreHarness.v",
        "hardware/src/cpu/CPUMIPS.v",
    ):
        if not (root / rel).is_file():
            gaps.append(rel)
    if not (root / "hardware/src/innerds").is_dir():
        gaps.append("hardware/src/innerds/")
    return gaps


def software_tree_gaps(sw: Path) -> list[str]:
    """proj7 player / sm entrypoints need these built software blobs."""
    need = (
        "gios/gios.inst.bin",
        "bench/bench.inst.bin",
        "proj_app7/isr7.bin",
        "proj_app7/proj7.inst.bin",
    )
    return [rel for rel in need if not (sw / rel).is_file()]


def skip_line(reason: str) -> str:
    return f"SKIP: {reason} (0 cases run)"


def print_skip(reason: str) -> int:
    """Print SKIP and return 0 for test mains."""
    print(skip_line(reason))
    return 0
