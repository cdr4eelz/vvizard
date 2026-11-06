#!/usr/bin/env python3
"""Fast CPU backend smoke tests: --cpu=fast must match Unicorn on simple hex programs.

Unicorn remains the default. These tests only exercise the optional backend.
"""
import os, subprocess, sys, tempfile
from vvprog import prog

HERE = os.path.dirname(os.path.abspath(__file__))
VV = os.path.join(os.path.dirname(HERE), "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
BASE = ["--sync", "--no-window", "--frame-clock=emulated", "--quiet-mmio",
        "--no-random-ddr", "--no-bios", "--max-insns", "1000000", "--no-halt-on-quirk"]


def binfile(name):
    return prog(name)[0]


def run_cpu(cpu, name, addr, *extra):
    return subprocess.run(
        [VV, *BASE, f"--cpu={cpu}", "--load", f"{binfile(name)}@{addr:#x}",
         "--entry", f"{addr:#x}", *extra],
        stdin=subprocess.DEVNULL, capture_output=True, env=ENV)


def expect_pass(r, label):
    err = r.stderr.decode(errors="replace")
    if r.returncode != 0:
        print(f"  {label}: FAIL rc={r.returncode}\n{err[-2000:]}")
        return False
    if "[expect]" in err and "FAIL" in err:
        print(f"  {label}: FAIL expect\n{err[-2000:]}")
        return False
    print(f"  {label}: PASS")
    return True


def t_help_mentions_cpu():
    r = subprocess.run([VV, "--help"], capture_output=True, env=ENV)
    text = (r.stdout + r.stderr).decode(errors="replace")
    ok = "--cpu=" in text and "fast" in text and "unicorn" in text
    print(f"  help_cpu: {'PASS' if ok else 'FAIL'}")
    return ok


def t_default_is_unicorn_loop():
    r = subprocess.run(
        [VV, *BASE, "--load", f"{binfile('loop')}@0x40000000", "--entry", "0x40000000",
         "--expect", "t0=30", "--expect", "t2=0x11e"],
        stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
    return expect_pass(r, "default_unicorn_loop")


def t_fast_loop():
    r = run_cpu("fast", "loop", 0x40000000, "--expect", "t0=30", "--expect", "t2=0x11e")
    return expect_pass(r, "fast_loop")


def t_unicorn_loop_explicit():
    r = run_cpu("unicorn", "loop", 0x40000000, "--expect", "t0=30", "--expect", "t2=0x11e")
    return expect_pass(r, "unicorn_loop")


def t_fast_alias():
    r = run_cpu("fast", "alias", 0xC0000180, "--expect", "t3=0xbeef", "--expect", "t4=0xbeef")
    return expect_pass(r, "fast_alias")


def main():
    if not os.path.isfile(VV):
        print(f"FAIL: missing binary {VV} (build first)", file=sys.stderr)
        return 1
    fails = 0
    for t in (t_help_mentions_cpu, t_default_is_unicorn_loop, t_unicorn_loop_explicit,
              t_fast_loop, t_fast_alias):
        fails += not t()
    print(f"{'PASS' if not fails else 'FAIL'}: {5 - fails}/5")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
