#!/usr/bin/env python3
"""make test-cpu-rtl entrypoint.

Optional deps (Icarus, MIPS as/objcopy, CS150 CoreHarness RTL) missing → SKIP exit 0.
Present deps that then fail → hard FAIL. Missing vvizard → FAIL (run make).
"""
import os, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "rtl"))
# Re-exec driver with same argv so CS150_ROOT from make env works
import run_rtl_diff
if __name__ == "__main__":
    # Allow CS150_ROOT from environment without requiring flag
    sys.exit(run_rtl_diff.main(sys.argv[1:]))
