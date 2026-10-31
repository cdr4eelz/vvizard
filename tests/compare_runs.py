#!/usr/bin/env python3
"""Compare two vvizard builds / modes run for run (docs/MULTITHREADING-PLAN.md section 10).

  python3 tests/compare_runs.py "A_CMD" "B_CMD" [--window] [--insns N] [--keep DIR]

A_CMD / B_CMD: a vvizard binary plus options, e.g. "./vvizard --sync" "./vvizard --threads=on".
Every case runs with --frame-clock=emulated (deterministic) and --event-trace; the UART stdout,
the event trace, the exit status, --dump-frame / script PPMs and the stderr lines that describe guest
state ([hw] counters, [irq], registers, [exit], [expect], [quirk] reports, [gpu]) must be byte-identical.
Timing lines ([frame], [run], [quirk] emulating / warn on, [hw] GPU) are left out.  Cases: every
tests/hw and tests/*.s program, and the proj7 player (default program) headless (and with
--window: in a window), plain and with the test_player.py key script."""
import os, re, shlex, subprocess, sys, tempfile, glob, filecmp

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vvprog import prog

ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
SKIP = re.compile(r"^\[(frame|run|load|bios|app|ddr|script|disp|raylib)\]|^\[quirk\] (emulating|warn on|halt on)|^\[hw\] GPU \(threaded\)")


def cases(window, insns):
    hw = ["--no-window", "--frame-clock=emulated", "--no-random-ddr", "--no-bios", "--max-insns", "1000000", "--no-halt-on-quirk"]
    out = []
    for s in sorted(glob.glob(os.path.join(HERE, "hw", "*.s")) + glob.glob(os.path.join(HERE, "*.s"))):
        name = os.path.relpath(s, HERE)[:-2]
        if name == "hw/isr": continue
        b, at = prog(name)
        extra = ["--load", f"{prog('hw/isr')[0]}@0xc0000180"] if name.startswith("hw/") else []
        out.append((name, hw + ["--load", f"{b}@0x{at:08x}", "--entry", f"0x{at:08x}", "--uart-in", "ab\\r"] + extra, None))
    gp = hw + ["--load", f"{prog('gpcmds')[0]}@0x12003000", "--load", f"{prog('gplaunch')[0]}@0x40000000", "--entry", "0x40000000"]
    out.append(("gpcmds", gp + ["--dump-frame", "{T}/f.ppm"], ["f.ppm"]))
    p7 = ["--frame-clock=emulated", "--random-ddr=12345", "--no-halt-on-quirk", "--max-insns", str(insns)]
    out.append(("proj7 headless", ["--no-window"] + p7 + ["--dump-frame", "{T}/f.ppm"], ["f.ppm"]))
    script = "\n".join(f"f{8 + 3 * k} key {c}" for k, c in enumerate("iiijjjlll,,,ooo")) + "\nf40 dump {T}/s40.ppm\nf60 dump {T}/s60.ppm\n"
    out.append(("proj7 keys", ["--no-window"] + p7[:-1] + ["70000000", "--input-script", "{T}/script.txt"], ["s40.ppm", "s60.ppm"], script))
    if window:
        out.append(("proj7 window", ["--window"] + p7 + ["--dump-frame", "{T}/f.ppm"], ["f.ppm"]))
    return [c if len(c) == 4 else c + (None,) for c in out]


def one(cmd, args, tdir, script):
    os.makedirs(tdir, exist_ok=True)
    if script: open(os.path.join(tdir, "script.txt"), "w").write(script.replace("{T}", tdir))
    ev = os.path.join(tdir, "ev.txt")
    a = [x.replace("{T}", tdir) for x in args]
    r = subprocess.run(shlex.split(cmd) + a + ["--event-trace", ev], stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
    err = [l for l in r.stderr.decode(errors="replace").splitlines() if not SKIP.match(l)]
    return r.returncode, r.stdout, open(ev, "rb").read() if os.path.exists(ev) else b"", err


def main():
    av = sys.argv[1:]
    window = "--window" in av; av = [x for x in av if x != "--window"]
    insns = 50000000
    if "--insns" in av: i = av.index("--insns"); insns = int(av[i + 1]); del av[i:i + 2]
    keep = None
    if "--keep" in av: i = av.index("--keep"); keep = av[i + 1]; del av[i:i + 2]
    A, B = av
    T = keep or tempfile.mkdtemp(prefix="vv_cmp_")
    fails = 0
    for k, (name, args, files, script) in enumerate(cases(window, insns)):
        ta, tb = os.path.join(T, f"{k}a"), os.path.join(T, f"{k}b")
        ra, rb = one(A, args, ta, script), one(B, args, tb, script)
        diff = [w for w, x, y in zip(("rc", "stdout", "events", "stderr"), ra, rb) if x != y]
        for f in files or []:
            if not (os.path.exists(os.path.join(ta, f)) and filecmp.cmp(os.path.join(ta, f), os.path.join(tb, f), shallow=False)):
                diff.append(f)
        nev = ra[2].count(b"\n")
        print(f"  {name:22s} {'SAME' if not diff else 'DIFF ' + ','.join(diff):24s} ({len(ra[1])} UART bytes, {nev} events)")
        fails += bool(diff)
    print(f"{'IDENTICAL' if not fails else 'DIFFERENT'}: {fails} of {k + 1} cases differ  ({A!r} vs {B!r}; files in {T})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
