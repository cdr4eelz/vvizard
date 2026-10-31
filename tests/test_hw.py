#!/usr/bin/env python3
"""Hardware-model tests (make test-hw): tests/hw/*.s against the cycle-timed device model (src/hw.c,
src/dcache.c) and the guest-bug detectors (warn by default, stop with --halt-on-quirk).
Each program counts EXPECTs in $s3 and mismatches in $s5 (tests/hw/hw.inc)."""
import os, re, subprocess, sys
from vvprog import prog

HERE = os.path.dirname(os.path.abspath(__file__))
VV = os.path.join(os.path.dirname(HERE), "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
ENV.pop("NO_COLOR", None)
# Explicit settings (vvizard's own defaults are for the proj7 player: no limit, --halt-on-quirk);
# a --load replaces the default program set.
BASE = ["--no-window", "--quiet-mmio", "--no-random-ddr", "--no-bios", "--max-insns", "1000000", "--no-halt-on-quirk"]


def run(name, nexp, *extra, isr=False, stdin=None):
    """Run tests/hw/NAME.s at 0x40000000 (+ tests/hw/isr.s at PC_ISR); expects $s5=0, $s3=nexp."""
    args = [VV, *BASE, "--load", f"{prog('hw/' + name)[0]}@0x40000000", "--entry", "0x40000000"]
    if isr:
        args += ["--load", f"{prog('hw/isr')[0]}@0xc0000180"]
    if nexp is not None:
        args += ["--expect", "s5=0", "--expect", f"s3={nexp}"]
    r = subprocess.run(args + list(extra), input=stdin or b"", capture_output=True, env=ENV)
    r.err = r.stderr.decode(errors="replace")
    return r


def bugs(r):
    """guest-bug categories reported (warnings and halts)"""
    return sorted(set(re.findall(r"^\[quirk\] ([a-z-]+) at pc=.*\[guest bug\]$", r.err, re.M)) |
                  set(re.findall(r"^\[stop\] --halt-on-quirk: ([a-z-]+) ", r.err, re.M)))


def halted(r, cat):
    return r.returncode != 0 and f"[stop] --halt-on-quirk: {cat} " in r.err


def hwline(r):
    m = re.search(r"^\[hw\] .*$", r.err, re.M)
    return m.group(0) if m else ""


# ---- A: hardware fidelity ----
def t_uart_tx():      # TX_READY low for 4773 cycles, Cause bit 11 when ready again; nothing lost
    r = run("uart_tx", 6)
    return r.returncode == 0 and r.stdout == b"AB" and not bugs(r)
def t_uart_rx():      # Cause bit 10 = edge latched; mtc0 only clears; read clears Valid; --uart-in paced
    r = run("uart_rx", 8, "--uart-in", "AB")
    return r.returncode == 0 and not bugs(r) and "rx 2 (overrun 0)" in hwline(r)
def t_uart_rx_hold(): # one holding register: overwrite (rx-overrun reported, run continues).  Bytes on
    # stdin (not --uart-in, which waits for each byte to be read) sent back to back (--uart-rx-gap 0)
    r = run("uart_rxover", 2, "--uart-rx-gap", "0", stdin=b"XYZ")
    return r.returncode == 0 and bugs(r) == ["rx-overrun"] and "rx 3 (overrun 2)" in hwline(r)
def t_pf():           # PF_FRAME latched at the frame end (865800 cycles), Cause bit 13
    r = run("pf", 5)
    return r.returncode == 0 and not bugs(r)
def t_pf_period():    # --pf-period 0: switches at once (the pf.s 2nd EXPECT then fails: s5 != 0)
    r = run("pf", None, "--pf-period", "0", "--max-insns", "200000", "--expect", "t2=0x10400000")
    return "[expect] $t2 = 0x10400000" in r.err and "PASS" in r.err.split("[expect] $t2")[1].splitlines()[0]
def t_timer():        # Count == Compare -> bit 15; Compare write clears it; ISR entry, EPC
    r = run("timer", 5, isr=True)
    return r.returncode == 0 and not bugs(r)
def t_irq_defer():    # CPUMIPS.v:114: no entry right after an mtc0, a branch or a taken branch's target
    r = run("irq_defer", 8, isr=True)
    return r.returncode == 0 and not bugs(r)
def t_gp_status():    # graphics.h gstate_s layout: fault 15, frame 13:8; Cause bit 12 on launch
    r = run("gpstat", 5)
    return r.returncode == 0 and not bugs(r)
def t_reset_regs():   # no $sp preset: reads 0 and warns (undef-reg); --sp presets it, no warning
    a = run("reset_regs", 1, "--expect", "s0=0")
    b = run("reset_regs", 1, "--sp", "0x50004000", "--expect", "s0=0x50004000")
    return a.returncode == 0 and bugs(a) == ["undef-reg"] and "$sp" in a.err \
        and b.returncode == 0 and not bugs(b)
def t_legacy_io():    # --legacy-io: TX always ready (both bytes out), PF_FRAME immediate
    a = run("uart_txbusy", None, "--legacy-io", "--expect", "t2=1")
    b = run("pf", None, "--legacy-io", "--max-insns", "200000", "--expect", "t2=0x10400000")
    return a.returncode == 0 and a.stdout == b"AB" and not bugs(a) \
        and "[expect] $t2 = 0x10400000 (want 0x10400000) PASS" in b.err


# ---- B: guest-bug detectors (warn by default; --halt-on-quirk stops; --quiet-quirk silences) ----
def t_tx_busy():
    w = run("uart_txbusy", 1)
    h = run("uart_txbusy", None, "--halt-on-quirk")
    q = run("uart_txbusy", 1, "--quiet-quirk=tx-busy")
    return w.returncode == 0 and w.stdout == b"A" and bugs(w) == ["tx-busy"] and "lost 1" in hwline(w) \
        and halted(h, "tx-busy") and q.returncode == 0 and not bugs(q)
def t_rx_overrun():
    h = run("uart_rxover", None, "--uart-rx-gap", "0", "--halt-on-quirk", stdin=b"XYZ")
    return halted(h, "rx-overrun")
def t_irq_storm():    # ISR returns with the timer cause pending (jr+mtc0 Status, and via eret)
    a = run("irq_storm", 5, isr=True)
    b = run("irq_storm_eret", 5, isr=True)
    h = run("irq_storm", None, "--halt-on-quirk", isr=True)
    return a.returncode == 0 and bugs(a) == ["irq-storm"] and "irq storms 2" in hwline(a) \
        and b.returncode == 0 and bugs(b) == ["irq-storm"] and "eret with Cause" in b.err \
        and halted(h, "irq-storm")
def t_undef_reg():
    h = run("reset_regs", None, "--halt-on-quirk")
    return halted(h, "undef-reg")
def t_dcache_stale():  # stale line after a GPU write: HW (default) reads the old 0; -dcache-stale reads DDR
    a = run("dcache", 2, "--expect", "s0=0")
    b = run("dcache", 2, "--quirks=-dcache-stale", "--expect", "s0=0x345678")
    h = run("dcache", None, "--halt-on-quirk")
    return a.returncode == 0 and bugs(a) == ["dcache-stale"] and b.returncode == 0 and bugs(b) == ["dcache-stale"] \
        and halted(h, "dcache-stale")


# ---- C: output streams and --color ----
def t_streams():      # stdout = guest UART bytes only (also with --trace-regs); vvizard's own text on stderr
    r = run("uart_tx", 6, "--trace-regs", "--color=never")
    return r.returncode == 0 and r.stdout == b"AB" and "[trace]" in r.err and "[run]" in r.err \
        and "---- registers" in r.err and "\x1b" not in r.err
def t_color():        # auto (not a tty) = off; always: green UART run on stdout, [tags]/register names on stderr
    a = run("uart_tx", 6)
    w = run("uart_tx", 6, "--color")
    green = re.fullmatch(rb"\x1b\[1;(?:32|38;5;46)mAB\x1b\[0m", w.stdout)
    bad = run("uart_tx", None, "--color=sometimes")
    return a.stdout == b"AB" and "\x1b" not in a.err and w.returncode == 0 and bool(green) \
        and "\x1b[1;31m[stop]\x1b[0m" in w.err and "\x1b[1;36mzero\x1b[0m=" in w.err \
        and "\x1b[1;36mpc\x1b[0m=" in w.err and bad.returncode == 2 and bad.stdout == b""


TESTS = [t_uart_tx, t_uart_rx, t_uart_rx_hold, t_pf, t_pf_period, t_timer, t_irq_defer, t_gp_status, t_reset_regs,
         t_legacy_io, t_tx_busy, t_rx_overrun, t_irq_storm, t_undef_reg, t_dcache_stale,
         t_streams, t_color]


def main():
    fails = 0
    for t in TESTS:
        ok = t(); fails += not ok
        print(f"  {t.__name__[2:]:14s} {'PASS' if ok else 'FAIL'}")
    print(f"{'PASS' if not fails else 'FAIL'}: {len(TESTS) - fails}/{len(TESTS)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
