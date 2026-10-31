#!/usr/bin/env python3
"""Threaded-mode tests (make test-threads; docs/MULTITHREADING-PLAN.md section 10).

The rest of make test runs --sync (one thread, the GPU completes inside the GP_CODE store).  Here:
  A. the threaded GPU model: ready bits low until launch + pixels/4 + 32 x commands, Cause bit 12 at
     that edge, a GP_CODE store while busy dropped (gp-busy), D-cache snapshot at launch;
  B. determinism: --event-trace (every guest-visible device event with its instruction count) and
     the UART output are byte-identical between two threaded runs, between threaded runs with and
     without a window (when $DISPLAY is set), and between --sync and threaded for programs that do
     not use the GPU (so threading changes nothing but GPU timing);
  C. the proj7 player test (test_player.py) with --threads=on."""
import os, subprocess, sys, tempfile
from vvprog import prog

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VV = os.path.join(ROOT, "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
BASE = ["--no-window", "--frame-clock=emulated", "--quiet-mmio", "--no-random-ddr", "--no-bios", "--max-insns", "1000000", "--no-halt-on-quirk"]
TMP = tempfile.mkdtemp(prefix="vv_threads_")
_n = [0]


def run(name, *extra, threads="on", base=BASE, isr=False, stdin=b""):
    """tests/NAME.s (hw/NAME for hw tests) at its load address; returns (proc, stderr, event trace)."""
    _n[0] += 1; ev = os.path.join(TMP, f"ev{_n[0]}.txt")
    args = [VV, f"--threads={threads}", *base, "--event-trace", ev]
    if name:
        b, at = prog(name)
        args += ["--load", f"{b}@0x{at:08x}", "--entry", f"0x{at:08x}"]
    if isr:
        args += ["--load", f"{prog('hw/isr')[0]}@0xc0000180"]
    r = subprocess.run(args + list(extra), input=stdin, capture_output=True, env=ENV)
    return r, r.stderr.decode(errors="replace"), open(ev, "rb").read()


def ok_expect(err, n):
    return "[expect] $s5 = 0x00000000 (want 0x00000000) PASS" in err and f"(want 0x{n:08x}) PASS" in err


# ---- A: threaded GPU model ----
def t_gp_async():       # busy window, ready edge + bit 12, drop while busy, pixels drawn
    r, err, ev = run("hw/gpstat_async", "--expect", "s5=0", "--expect", "s3=10")
    return r.returncode == 0 and ok_expect(err, 10) and "[quirk] gp-busy at pc=" in err \
        and "1 dropped while busy" in err and b"gp-drop" in ev
def t_gp_busy_halt():   # --halt-on-quirk=+gp-busy stops at the dropped launch (exit 4)
    r, err, _ = run("hw/gpstat_async", "--halt-on-quirk=gp-busy")
    return r.returncode == 4 and "[stop] --halt-on-quirk: gp-busy " in err
def t_gp_sync_ready():  # --sync: GPU always ready, the drop never happens (gpstat_async's busy EXPECTs fail)
    r, err, ev = run("hw/gpstat_async", "--expect", "s5=0", threads="off")
    return r.returncode == 3 and "gp-busy" not in err.split("[run]")[1] and b"gp-drop" not in ev
def t_dcache_async():   # stale after GP_WAIT (snapshot at launch); -dcache-stale reads DDR
    a, ea, _ = run("hw/dcache_async", "--expect", "s0=0", "--expect", "s5=0")
    b, eb, _ = run("hw/dcache_async", "--quirks=-dcache-stale", "--expect", "s0=0x345678", "--expect", "s5=0")
    return a.returncode == 0 and "[quirk] dcache-stale at" in ea and b.returncode == 0
def t_gpcmds():         # the GPU command test (one probe per command) drawn by the worker
    ppm = os.path.join(TMP, "gp.ppm")
    r, err, _ = run(None, "--load", f"{prog('gpcmds')[0]}@0x12003000", "--load", f"{prog('gplaunch')[0]}@0x40000000",
                    "--entry", "0x40000000", "--dump-frame", ppm)
    chk = subprocess.run([sys.executable, os.path.join(HERE, "check_gp.py"), ppm], capture_output=True)
    return r.returncode == 0 and chk.returncode == 0


# ---- B: determinism ----
NO_GPU = [("hw/uart_tx", {}), ("hw/timer", {"isr": True}), ("hw/pf", {}), ("hw/irq_storm", {"isr": True}),
          ("hw/uart_rx", {"stdin": b"xyz"}), ("uart", {}), ("hw/irq_defer", {"isr": True})]
def t_same_without_gpu():
    bad = []
    for name, kw in NO_GPU:
        a, _, eva = run(name, threads="off", **kw)
        b, _, evb = run(name, threads="on", **kw)
        if a.stdout != b.stdout or eva != evb or a.returncode != b.returncode: bad.append(name)
    if bad: print("    differ:", bad)
    return not bad
PLAYER = ["--no-window", "--frame-clock=emulated", "--random-ddr=7", "--no-halt-on-quirk", "--max-insns", "20000000"]
def t_repeatable():     # threaded proj7: two runs, and headless vs window, give the same events
    a, _, eva = run(None, base=PLAYER)
    b, _, evb = run(None, base=PLAYER)
    same = a.returncode == 0 and a.stdout == b.stdout and eva == evb and b"gp-launch" in eva
    if os.environ.get("DISPLAY"):
        w, _, evw = run(None, base=["--window"] + PLAYER[1:])
        same = same and w.stdout == a.stdout and evw == eva
    return same


# ---- C: player ----
def t_player():
    r = subprocess.run([sys.executable, os.path.join(HERE, "test_player.py")], capture_output=True, text=True,
                       env=dict(ENV, VV_PLAYER_THREADS="--threads=on"))
    return r.returncode == 0


TESTS = [t_gp_async, t_gp_busy_halt, t_gp_sync_ready, t_dcache_async, t_gpcmds, t_same_without_gpu, t_repeatable, t_player]


def main():
    fails = 0
    for t in TESTS:
        ok = t(); fails += not ok
        print(f"  {t.__name__[2:]:18s} {'PASS' if ok else 'FAIL'}")
    print(f"{'PASS' if not fails else 'FAIL'}: {len(TESTS) - fails}/{len(TESTS)}")
    if not fails:
        import shutil; shutil.rmtree(TMP)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
