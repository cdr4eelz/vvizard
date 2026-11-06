#!/usr/bin/env python3
"""--ddr-stalls[=R,W]: caches+miss stalls only when enabled; defaults R=16 W=3; hits=0."""
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
    delta = g.get("s2", 999)
    if delta > 0x7FFFFFFF:
        delta = delta - (1 << 32)
    if abs(delta) > 8:
        print(f"  off_fast: FAIL expected tiny cycle-inst delta got {g} ({delta})"); return False
    print(f"  off_fast: PASS (delta={delta}, no [ddr] line)")
    return True


def t_on_defaults_16_3():
    """Bare --ddr-stalls and --ddr-stalls=16,3 both use read=16 write=3."""
    ok = True
    for label, extra in (
        ("bare", ["--ddr-stalls"]),
        ("eq", ["--ddr-stalls=16,3"]),
        ("space", ["--ddr-stalls", "16,3"]),
    ):
        for cpu in ("fast", "unicorn"):
            r = run("--cpu=" + cpu, *extra)
            err = r.stderr.decode(errors="replace")
            if r.returncode != 0:
                print(f"  on_{label}_{cpu}: FAIL rc={r.returncode}\n{err[-800:]}"); ok = False; continue
            if "ddr-stalls=16,3" not in err:
                print(f"  on_{label}_{cpu}: FAIL run line want ddr-stalls=16,3\n{err[-400:]}"); ok = False; continue
            m = re.search(
                r"\[ddr\] stalls on \(read=(\d+) write=(\d+); hit=0\): I\$ misses (\d+) D\$ misses (\d+) "
                r"D\$ writes (\d+); stall cycles (\d+), host cycles (\d+)",
                err,
            )
            if not m:
                print(f"  on_{label}_{cpu}: FAIL no [ddr] summary\n{err[-800:]}"); ok = False; continue
            rd, wr, im, dm, dw, stall, host = map(int, m.groups())
            if rd != 16 or wr != 3:
                print(f"  on_{label}_{cpu}: FAIL want read=16 write=3 got {m.group(0)}"); ok = False; continue
            if dm < 8 or dw < 1 or stall < 8 * 16 + 3:
                print(f"  on_{label}_{cpu}: FAIL weak stalls dm={dm} dw={dw} stall={stall}"); ok = False; continue
            g = regs(err)
            if g.get("s2", 0) < 100 or g["s0"] <= g["s1"]:
                print(f"  on_{label}_{cpu}: FAIL counters {g}"); ok = False; continue
            print(f"  on_{label}_{cpu}: PASS (D$miss={dm} write={dw} stall={stall} "
                  f"CNT_CYCLE={g['s0']} CNT_INST={g['s1']})")
    return ok


def t_custom_pair():
    r = run("--cpu=fast", "--ddr-stalls=10,2")
    err = r.stderr.decode(errors="replace")
    if r.returncode != 0 or "ddr-stalls=10,2" not in err:
        print(f"  custom: FAIL\n{err[-600:]}"); return False
    m = re.search(r"\[ddr\] stalls on \(read=10 write=2; hit=0\)", err)
    if not m:
        print(f"  custom: FAIL summary\n{err[-400:]}"); return False
    # 8 misses * 10 + 1 write * 2 = 82
    m2 = re.search(r"stall cycles (\d+)", err)
    stall = int(m2.group(1)) if m2 else 0
    if stall < 82:
        print(f"  custom: FAIL stall={stall} want >=82"); return False
    print(f"  custom: PASS (stall={stall})")
    return True


def t_bad_arg():
    r = run("--cpu=fast", "--ddr-stalls=nope")
    if r.returncode == 0:
        print("  bad_arg: FAIL expected nonzero"); return False
    err = (r.stderr + r.stdout).decode(errors="replace")
    if "ddr-stalls" not in err.lower() and "expected" not in err.lower():
        print(f"  bad_arg: FAIL no error message\n{err[-400:]}"); return False
    print("  bad_arg: PASS")
    return True


def t_help_mentions_ddr_stalls():
    r = subprocess.run([VV, "--help"], capture_output=True, env=ENV)
    err = (r.stdout + r.stderr).decode(errors="replace")
    ok = "--ddr-stalls" in err and "16" in err and "3" in err
    print(f"  help_ddr_stalls: {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    if not os.path.isfile(VV):
        print(f"FAIL: missing {VV}", file=sys.stderr); return 1
    tests = [t_help_mentions_ddr_stalls, t_off_no_ddr_line, t_on_defaults_16_3,
             t_custom_pair, t_bad_arg]
    fails = sum(not t() for t in tests)
    print(f"{'PASS' if not fails else 'FAIL'}: {len(tests) - fails}/{len(tests)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
