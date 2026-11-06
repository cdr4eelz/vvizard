#!/usr/bin/env python3
"""SKIP vs FAIL for shared optional_deps used by player/sm/rtl entrypoints."""
from __future__ import annotations
import os, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import optional_deps as od


def test_software_gaps_empty_tree():
    td = Path(tempfile.mkdtemp())
    gaps = od.software_tree_gaps(td)
    assert gaps, gaps
    assert "gios/gios.inst.bin" in gaps


def test_skip_line_not_pass():
    s = od.skip_line("missing tool(s): iverilog")
    assert s.startswith("SKIP:")
    assert "0 cases run" in s
    assert "PASS" not in s


def test_cs150_gaps():
    td = Path(tempfile.mkdtemp())
    assert "CoreHarness.v" in ",".join(od.cs150_gaps(td))


def test_print_skip_returns_0(capsys=None):
    rc = od.print_skip("demo reason")
    assert rc == 0


def main():
    fails = 0
    for t in (test_software_gaps_empty_tree, test_skip_line_not_pass, test_cs150_gaps, test_print_skip_returns_0):
        try:
            t()
            print(f"  {t.__name__}: PASS")
        except Exception as e:
            print(f"  {t.__name__}: FAIL {e}")
            fails += 1
    print(f"{'PASS' if not fails else 'FAIL'}: optional-deps-entry {4 - fails}/4")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
