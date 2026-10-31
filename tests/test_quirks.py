#!/usr/bin/env python3
"""Quirk tests (make test-quirks).

1. Detection: one tiny program per category (loaded at 0x40000000, ends in `break`).  With
   --halt-on-quirk=all each 'bad' program must stop with exit 4 and '[quirk] NAME at'; each clean
   program must run with exit 0 and no quirk.  Without a detection flag there is never a
   '[quirk] NAME at' line, and --halt-on-quirk=all,-NAME must not halt on NAME.  Plain
   --halt-on-quirk reports only emulated quirks, so it must not halt on the default-off
   (fixed) jalr-rd / lb-sign.
2. Emulation: each quirk ON (--quirks=+NAME) must give the MIPS150 result and OFF
   (--quirks=-NAME) the standard MIPS32 / Unicorn result (register values via --expect, stdout,
   or exit code), plus the CLI/env interface (VVIZARD_QUIRKS, --no-quirks, --list-quirks).
Programs: tests/quirk/det_*.s and emu_*.s, built by the Makefile (make tests; tests/vvprog.py).
These snippets are about the CPU quirks, so the guest-bug detectors (which warn by default; some
snippets read never-written registers or store to UART TX back to back) are silenced with
--quiet-quirk; tests/test_hw.py covers them."""
import os, re, subprocess, sys, tempfile
from vvprog import prog

HERE = os.path.dirname(os.path.abspath(__file__))
VV = os.path.join(os.path.dirname(HERE), "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
# name (tests/quirk/det_NAME.s), expected quirk (None = must not trip)
PROGS = [
    ("lb_neg", 'lb-sign'),
    ("lb_pos", None),
    ("lh_neg", 'lb-sign'),
    ("jalr_rd", 'jalr-rd'),
    ("jalr_ok", None),
    ("unm_load", 'unmapped'),
    ("unm_store", 'unmapped'),
    ("unm_fetch", 'unmapped-fetch'),
    ("mmio_fetch", 'unmapped-fetch'),
    ("bios_store", 'bios-store'),
    ("unaligned", 'unaligned'),
    ("lwl", 'lwlr'),
    ("overflow", 'overflow'),
    ("add_ok", None),
    ("hilo", 'hilo'),
    ("syscall", 'syscall'),
    ("mmio_lane", 'mmio-lane'),
    ("uart_ok", None),
    ("load_use", 'load-use'),
    ("load_use_ok", None),
    ("bal_untaken", 'bal-untaken'),
    ("reserved", 'reserved'),
    ("cop0", 'cop0'),
    ("ddr2", 'ddr2'),          # run from ISCR (not BIOS)
]

# name (tests/quirk/emu_NAME.s), quirk, ON check, OFF check.  A check is a list of REG=VAL, or
# "exc" (standard MIPS CPU exception), "rc!=0" (fault), "rc=0", or b"stdout bytes".
EMU = [
    ("jalr_rd", "jalr-rd", ["ra=0x40000014", "t1=0"], ["t1=0x40000014", "ra=0"]),
    ("lb_sign", "lb-sign", ["t2=0x80", "t3=0x8001"], ["t2=0xffffff80", "t3=0xffff8001"]),
    ("unm_load", "unmapped", ["t1=0", "t2=6"], "rc!=0"),
    ("unm_fetch", "unmapped-fetch", "rc=0", "rc!=0"),
    ("bios_store", "bios-store", ["t2=0"], ["t2=0x1234"]),
    ("ddr2", "ddr2", ["t2=0"], ["t2=0x55"]),
    ("unaligned", "unaligned", ["t2=0", "t4=0x11223344", "t0=0x50000000"], "exc"),
    ("lwlr", "lwlr", ["t2=0", "t0=0x50000000"], ["t2=0x22334477"]),
    ("overflow", "overflow", ["t2=0x80000000", "a0=0x7fffffff", "a1=1"], "exc"),
    ("hilo", "hilo", ["t2=0"], ["t2=15"]),
    ("syscall", "syscall", ["t2=2"], "exc"),
    ("movz", "reserved", ["t2=0"], ["t2=7"]),
    ("tgei", "reserved", ["t2=5"], ["t2=7"]),
    ("cop0", "cop0", ["t2=0", "t3=0xffff"], ["t2=0x1234"]),
    ("mmio_lane", "mmio-lane", b"\0", b"A"),
    ("load_use", "load-use", ["t3=1", "t2=0x99"], ["t3=0x99", "t2=0x99"]),
    ("bal_untaken", "bal-untaken", ["ra=0"], ["ra=0x40000010"]),
]


BUGS = "dcache-stale,tx-busy,rx-overrun,irq-storm,undef-reg"


def run(path, *opts, at=0x40000000, env=ENV):
    r = subprocess.run([VV, "--no-window", "--quiet-mmio", "--no-random-ddr", "--max-insns", "1000",
                        f"--quiet-quirk={BUGS}", *opts,
                        "--load", f"{path}@{at:#x}", "--entry", f"{at:#x}"],
                       stdin=subprocess.DEVNULL, capture_output=True, env=env)
    err = r.stderr.decode(errors="replace")
    return r.returncode, re.findall(r"^\[quirk\] ([a-z0-9-]+) at", err, re.M), r.stdout, err


def check(path, at, opts, want):
    exp = [f"--expect={e}" for e in want] if isinstance(want, list) else []
    rc, hits, out, err = run(path, *opts, *exp, at=at)
    if isinstance(want, list): ok = rc == 0
    elif want == "rc!=0": ok = rc != 0
    elif want == "exc": ok = re.search(r"^\[stop\] CPU exception", err, re.M) is not None   # standard MIPS trap
    elif want == "rc=0": ok = rc == 0
    else: ok = rc == 0 and out == want
    if not ok: print("     ", " ".join(opts), "rc", rc, out, [l for l in err.splitlines() if "FAIL" in l or "stop" in l])
    return ok


def main():
    fails = total = 0
    print("detection:")
    for name, want in PROGS:
        path, at = prog("quirk/det_" + name)
        rc, hits, _, _ = run(path, "--halt-on-quirk=all", at=at)
        ok = (rc == 4 and hits == [want]) if want else (rc == 0 and not hits)
        rc_off, hits_off, _, _ = run(path, at=at)          # no detection flag: never a report line
        ok = ok and not hits_off
        if want:                                           # excluding the category: no halt on it
            rc_x, hits_x, _, _ = run(path, f"--halt-on-quirk=all,-{want}", at=at)
            ok = ok and rc_x != 4 and want not in hits_x
            rc_d, hits_d, _, _ = run(path, "--halt-on-quirk", at=at)   # emulated set only
            ok = ok and ((want in hits_d) != (want in ("jalr-rd", "lb-sign")))
        total += 1; fails += not ok
        print(f"  {name:12s} expect {want or '-':14s} got rc={rc} {hits}  off: rc={rc_off} {'PASS' if ok else 'FAIL'}")
    print("emulation (quirk on / off):")
    for name, q, on, off in EMU:
        path, at = prog("quirk/emu_" + name)
        ok_on = check(path, at, [f"--quirks=+{q}"], on)
        ok_off = check(path, at, [f"--quirks=-{q}"], off)
        total += 1; fails += not (ok_on and ok_off)
        print(f"  {name:12s} {q:14s} on {'ok' if ok_on else 'BAD'}  off {'ok' if ok_off else 'BAD'}  {'PASS' if ok_on and ok_off else 'FAIL'}")
    print("interface:")
    hilo = prog("quirk/emu_hilo")[0]; jalr = prog("quirk/emu_jalr_rd")[0]
    cases = [
        ("default = hardware", lambda: check(hilo, 0x40000000, [], ["t2=0"]) and check(jalr, 0x40000000, [], ["t1=0x40000014"])),
        ("--no-quirks", lambda: check(hilo, 0x40000000, ["--no-quirks"], ["t2=15"])),
        ("--quirks=none,hilo", lambda: check(hilo, 0x40000000, ["--quirks=none,hilo"], ["t2=0"])),
        ("--quirks=all", lambda: check(jalr, 0x40000000, ["--quirks=all"], ["ra=0x40000014"])),
        ("env VVIZARD_QUIRKS=-hilo", lambda: run(hilo, "--expect=t2=15", env=dict(ENV, VVIZARD_QUIRKS="-hilo"))[0] == 0),
        ("env then CLI +hilo", lambda: run(hilo, "--quirks=+hilo", "--expect=t2=0", env=dict(ENV, VVIZARD_QUIRKS="-hilo"))[0] == 0),
        ("bad name -> rc 2", lambda: run(hilo, "--quirks=bogus")[0] == 2),
        ("--list-quirks", lambda: subprocess.run([VV, "--list-quirks"], capture_output=True, env=ENV, text=True).stdout.count("\n") == 22),
    ]
    for label, fn in cases:
        ok = fn(); total += 1; fails += not ok
        print(f"  {label:28s} {'PASS' if ok else 'FAIL'}")
    print(f"{'PASS' if not fails else 'FAIL'}: {total - fails}/{total}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
