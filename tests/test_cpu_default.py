#!/usr/bin/env python3
"""Default CPU backend must be fast; --cpu=unicorn remains explicit reference."""
from __future__ import annotations
import os, re, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
VV = str(ROOT / "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.environ.get("LD_LIBRARY_PATH", "") + ":" + os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
BASE = ["--sync", "--no-window", "--frame-clock=emulated", "--quiet-mmio",
        "--no-random-ddr", "--no-bios", "--max-insns", "200000", "--no-halt-on-quirk"]


def _loop_bin():
    sys.path.insert(0, str(HERE))
    from vvprog import prog
    return prog("loop")[0]


def _run(extra):
    bil = _loop_bin()
    return subprocess.run(
        [VV, *BASE, *extra, "--load", f"{bil}@0x40000000", "--entry", "0x40000000",
         "--expect", "t0=30", "--expect", "t2=0x11e"],
        capture_output=True, env=ENV)


def test_help_default_fast():
    r = subprocess.run([VV, "--help"], capture_output=True, env=ENV, text=True)
    text = r.stdout + r.stderr
    assert "--cpu=" in text
    # Must say default is fast (not unicorn)
    m = re.search(r"--cpu=unicorn\|fast.*?default\s+(\w+)", text, re.S)
    assert m, text[text.find("--cpu"):text.find("--cpu")+200]
    assert m.group(1) == "fast", m.group(1)


def test_default_run_line_is_fast():
    r = _run([])
    err = r.stderr.decode(errors="replace")
    assert r.returncode == 0, err[-500:]
    assert re.search(r"\[run\] cpu=fast\b", err), err


def test_default_matches_explicit_fast():
    rd = _run([])
    rf = _run(["--cpu=fast"])
    assert rd.returncode == 0 and rf.returncode == 0
    # Same expect success; run line both fast
    assert b"cpu=fast" in rd.stderr and b"cpu=fast" in rf.stderr


def test_explicit_unicorn_still_works():
    r = _run(["--cpu=unicorn"])
    err = r.stderr.decode(errors="replace")
    assert r.returncode == 0, err[-500:]
    assert re.search(r"\[run\] cpu=unicorn\b", err), err


def test_fast_threads_diag_not_silent():
    bil = _loop_bin()
    r = subprocess.run(
        [VV, *BASE, "--cpu=fast", "--threads=on", "--window",
         "--load", f"{bil}@0x40000000", "--entry", "0x40000000", "--max-insns", "100"],
        capture_output=True, env=ENV)
    err = r.stderr.decode(errors="replace")
    assert "forcing --sync" in err or "does not support threaded" in err, err[-600:]


def main():
    if not Path(VV).is_file():
        print(f"FAIL: missing {VV}", file=sys.stderr)
        return 1
    fails = 0
    for t in (test_help_default_fast, test_default_run_line_is_fast,
              test_default_matches_explicit_fast, test_explicit_unicorn_still_works,
              test_fast_threads_diag_not_silent):
        try:
            t()
            print(f"  {t.__name__}: PASS")
        except AssertionError as e:
            print(f"  {t.__name__}: FAIL {e}")
            fails += 1
        except Exception as e:
            print(f"  {t.__name__}: FAIL {type(e).__name__}: {e}")
            fails += 1
    n = 5
    print(f"{'PASS' if not fails else 'FAIL'}: cpu-default {n - fails}/{n}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
