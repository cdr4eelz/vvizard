#!/usr/bin/env python3
"""make test-cpu-rtl entrypoint. Fails hard if CS150_ROOT/tools missing (no fake PASS)."""
import os, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "rtl"))
# Re-exec driver with same argv so CS150_ROOT from make env works
import run_rtl_diff
if __name__ == "__main__":
    # Allow CS150_ROOT from environment without requiring flag
    sys.exit(run_rtl_diff.main(sys.argv[1:]))
