#!/usr/bin/env python3
"""Hex program tests (make test-hex): the pass conditions listed in NOTES.md "Test pass
conditions", plus tests/hwprobe (MIPS150 behaviour probe, must pass with the default quirks).
Each test runs with the default quirk set (= current hardware) and --halt-on-quirk off."""
import os, subprocess, sys, tempfile
from vvprog import prog

HERE = os.path.dirname(os.path.abspath(__file__))
VV = os.path.join(os.path.dirname(HERE), "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
TMP = tempfile.mkdtemp(prefix="vv_hex_")
# Explicit settings (vvizard's own defaults are for the proj7 player: no limit, --halt-on-quirk);
# a --load replaces the default program set.
BASE = ["--sync", "--no-window", "--frame-clock=emulated", "--quiet-mmio", "--no-random-ddr", "--no-bios", "--max-insns", "1000000", "--no-halt-on-quirk"]


def binfile(name):
    return prog(name)[0]


def vv(*args):
    return subprocess.run([VV, *BASE, *args], stdin=subprocess.DEVNULL, capture_output=True, env=ENV)


def ppm_px(path, x, y):
    d = open(path, "rb").read(); h = d.index(b"255\n") + 4
    return tuple(d[h + 3 * (y * 800 + x): h + 3 * (y * 800 + x) + 3])


def at(name, addr, *extra):
    return vv("--load", f"{binfile(name)}@{addr:#x}", "--entry", f"{addr:#x}", *extra)


def t_loop():   return at("loop", 0x40000000, "--expect", "t0=30", "--expect", "t2=0x11e").returncode == 0
def lcd_lines(r):
    """--dump-lcd output -> [line1 hex bytes, line2 hex bytes]"""
    return [bytes.fromhex(l.split(b"hex")[1].decode()) for l in r.stderr.splitlines() if l.startswith((b"[lcd] 1 ", b"[lcd] 2 "))]
def t_uart():   # 0x80000108 is no longer a UART TX alias: the 'i' lands in LCD[2]; TX_READY 0 after a store
    r = at("uart", 0x40000000, "--expect", "t2=0", "--dump-lcd")
    return r.returncode == 0 and r.stdout == b"H\n" and lcd_lines(r)[0][:3] == b"  i" and b"[guest bug]" not in r.stderr
LCD_REGS = ["t2=0", "t3=0", "t4=1", "t5=0", "t6=1", "t7=0", "t8=0"]
def t_lcd():    # tests/lcd.s: LCD MMIO lanes, line ends, 16 KiB alias, reads 0, no 0x100 register aliases
    ok = True
    for q, l1 in (("default", b"QiU\0X\0" + b" " * 9 + b"L"), ("-mmio-lane", b"QiUXXX" + b" " * 9 + b"L")):
        r = at("lcd", 0x40000000, "--dump-lcd", f"--quirks={q}", *[a for e in LCD_REGS for a in ("--expect", e)])
        ok &= r.returncode == 0 and r.stdout == b"\n" and lcd_lines(r) == [l1, b"a" + b" " * 14 + b"z"]
    return ok
def t_alias():  return at("alias", 0xC0000180, "--expect", "t3=0xbeef", "--expect", "t4=0xbeef").returncode == 0
def t_frame():
    f = os.path.join(TMP, "frame.ppm"); r = at("frame", 0x40000000, "--dump-frame", f)
    return r.returncode == 0 and ppm_px(f, 100, 50) == (255, 128, 0) and ppm_px(f, 131, 59) == (255, 128, 0) \
        and ppm_px(f, 99, 50) != (255, 128, 0) and ppm_px(f, 132, 59) != (255, 128, 0)
def t_prng():
    exp = "a0=0x56709234 a1=0x2b38491a s0=0xe28b8493 s1=0xdeadbeef s2=0xef76df74 s3=0x3bddb7dd s4=0x1 " \
          "s5=0xab s6=0xab000000 s7=0x0 t3=0x0ab00000 t5=0x9484c744"
    args = [a for e in exp.split() for a in ("--expect", e)]
    return at("prng", 0x40000000, "--max-insns", "300000", *args).returncode == 0
def t_gp():
    f = os.path.join(TMP, "gp.ppm")
    r = vv("--load", f"{binfile('gpcmds')}@0x12003000", "--load", f"{binfile('gplaunch')}@0x40000000",
           "--entry", "0x40000000", "--dump-frame", f)
    return r.returncode == 0 and subprocess.run([sys.executable, os.path.join(HERE, "check_gp.py"), f],
                                                capture_output=True).returncode == 0
def t_hwprobe():
    return at("hwprobe", 0x60000000, "--expect", "s5=0", "--expect", "s3=45").returncode == 0


TESTS = [t_loop, t_uart, t_lcd, t_alias, t_frame, t_prng, t_gp, t_hwprobe]


def main():
    fails = 0
    for t in TESTS:
        ok = t(); fails += not ok
        print(f"  {t.__name__[2:]:10s} {'PASS' if ok else 'FAIL'}")
    print(f"{'PASS' if not fails else 'FAIL'}: {len(TESTS) - fails}/{len(TESTS)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
