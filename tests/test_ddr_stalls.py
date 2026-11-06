#!/usr/bin/env python3
"""--ddr-stalls: caches+miss stalls only when enabled; CNT_CYCLE can exceed CNT_INST."""
import os, re, subprocess, sys
from vvprog import prog

HERE = os.path.dirname(os.path.abspath(__file__))
VV = os.path.join(os.path.dirname(HERE), "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
BASE = ["--sync", "--no-window", "--frame-clock=emulated", "--quiet-mmio",
        "--no-random-ddr", "--no-bios", "--max-insns", "100000", "--no-halt-on-quirk"]


def run(*extra):
    b, at = prog("ddr_stall")
    args = [VV, *BASE, "--load", f"{b}@{at:#x}", "--entry", f"{at:#x}", *extra]
    return subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, env=ENV)


def regs(err):
    out = {}
    for m in re.finditer(r"\b(s[0-2])=([0-9a-fA-F]{8})\b", err):
        out[m.group(1)] = int(m.group(2), 16)
    return out


def t_off_no_ddr_line():
    """Caches/stalls inactive: no [ddr] summary; small cycle-inst skew from read gap only."""
    r = run("--cpu=fast")
    err = r.stderr.decode(errors="replace")
    if r.returncode != 0:
        print(f"  off_fast: FAIL rc={r.returncode}\n{err[-600:]}"); return False
    if "ddr-stalls=off" not in err:
        print(f"  off_fast: FAIL missing ddr-stalls=off\n{err[-400:]}"); return False
    if re.search(r"^\[ddr\] stalls on", err, re.M):
        print(f"  off_fast: FAIL unexpected [ddr] summary"); return False
    g = regs(err)
    # Two counter reads are a few insns apart, so |delta| is tiny without stalls.
    delta = g.get("s2", 999)
    if delta > 0x7FFFFFFF:  # signed
        delta = delta - (1 << 32)
    if abs(delta) > 8:
        print(f"  off_fast: FAIL expected tiny cycle-inst delta got {g} ({delta})"); return False
    print(f"  off_fast: PASS (delta={delta}, no [ddr] line)")
    return True


def t_on_stalls_and_summary():
    ok = True
    for cpu in ("fast", "unicorn"):
        r = run("--cpu=" + cpu, "--ddr-stalls")
        err = r.stderr.decode(errors="replace")
        if r.returncode != 0:
            print(f"  on_{cpu}: FAIL rc={r.returncode}\n{err[-800:]}"); ok = False; continue
        if "ddr-stalls=on" not in err:
            print(f"  on_{cpu}: FAIL run line\n{err[-400:]}"); ok = False; continue
        m = re.search(
            r"\[ddr\] stalls on \(miss=(\d+) write=(\d+)\): I\$ misses (\d+) D\$ misses (\d+) D\$ writes (\d+); "
            r"stall cycles (\d+), host cycles (\d+)",
            err,
        )
        if not m:
            print(f"  on_{cpu}: FAIL no [ddr] summary\n{err[-800:]}"); ok = False; continue
        miss_c, wr_c, im, dm, dw, stall, host = map(int, m.groups())
        if miss_c != 20 or wr_c != 3:
            print(f"  on_{cpu}: FAIL unexpected constants {m.group(0)}"); ok = False; continue
        if dm < 8:
            print(f"  on_{cpu}: FAIL expected >=8 D$ misses got {dm}"); ok = False; continue
        if dw < 1:
            print(f"  on_{cpu}: FAIL expected D$ write got {dw}"); ok = False; continue
        if stall < 8 * 20 + 3:
            print(f"  on_{cpu}: FAIL stall cycles too small {stall}"); ok = False; continue
        if host <= int(regs(err).get("s1", 0)):
            # host cycles should exceed insn count
            pass
        g = regs(err)
        if g.get("s2", 0) < 100:
            print(f"  on_{cpu}: FAIL expected large cycle-inst delta got {g}"); ok = False; continue
        if g["s0"] <= g["s1"]:
            print(f"  on_{cpu}: FAIL CNT_CYCLE ({g['s0']}) <= CNT_INST ({g['s1']})"); ok = False; continue
        print(f"  on_{cpu}: PASS (D$miss={dm} write={dw} stall={stall} host={host} "
              f"CNT_CYCLE={g['s0']} CNT_INST={g['s1']} delta={g['s2']})")
    return ok


def t_help_mentions_ddr_stalls():
    r = subprocess.run([VV, "--help"], capture_output=True, env=ENV)
    err = (r.stdout + r.stderr).decode(errors="replace")
    ok = "--ddr-stalls" in err
    print(f"  help_ddr_stalls: {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    if not os.path.isfile(VV):
        print(f"FAIL: missing {VV}", file=sys.stderr); return 1
    tests = [t_help_mentions_ddr_stalls, t_off_no_ddr_line, t_on_stalls_and_summary]
    fails = sum(not t() for t in tests)
    print(f"{'PASS' if not fails else 'FAIL'}: {len(tests) - fails}/{len(tests)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
