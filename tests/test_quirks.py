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
Hex: mips-linux-gnu-as -EB (-mips1 for the detection set, -mips32r2 for the emulation set)."""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VV = os.path.join(os.path.dirname(HERE), "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
# name, expected quirk (None = must not trip), big-endian words
PROGS = [
    ("lb_neg", 'lb-sign', "3c085000 24090080 a1090000 00000000 810a0000 00000000 00000000 0000000d"),
    ("lb_pos", None, "3c085000 2409007f a1090000 00000000 810a0000 00000000 24097fff a5090002 850b0002 00000000 910c0000 00000000 0000000d"),
    ("lh_neg", 'lb-sign', "3c085000 34098001 a5090002 00000000 850b0002 00000000 0000000d"),
    ("jalr_rd", 'jalr-rd', "3c084000 35080010 01004809 00000000 00000000 00000000 0000000d"),
    ("jalr_ok", None, "3c084000 35080010 0100f809 00000000 00000000 00000000 0000000d"),
    ("unm_load", 'unmapped', "3c087000 8d090000 00000000 0000000d"),
    ("unm_store", 'unmapped', "ac090000 0000000d"),
    ("unm_fetch", 'unmapped-fetch', "3c089000 01000008 00000000 0000000d"),
    ("mmio_fetch", 'unmapped-fetch', "3c088000 01000008 00000000 0000000d"),
    ("bios_store", 'bios-store', "3c084000 ad080100 0000000d"),
    ("unaligned", 'unaligned', "3c085000 8d090002 00000000 0000000d"),
    ("lwl", 'lwlr', "3c085000 89090001 00000000 0000000d"),
    ("overflow", 'overflow', "3c087fff 3508ffff 21090001 0000000d"),
    ("add_ok", None, "24080005 01084820 01084822 250afff7 0000000d"),
    ("hilo", 'hilo', "24080003 01080018 00004812 0000000d"),
    ("syscall", 'syscall', "0000000c 0000000d"),
    ("mmio_lane", 'mmio-lane', "3c088000 24090041 a1090008 0000000d"),
    ("uart_ok", None, "3c088000 24090041 a109000b ad090008 0000000d"),
    ("load_use", 'load-use', "3c085000 8d090000 01295021 0000000d"),
    ("load_use_ok", None, "3c085000 8d090000 00000000 01295021 0000000d"),
    ("bal_untaken", 'bal-untaken', "24040001 04900001 00000000 0000000d"),
    ("reserved", 'reserved', "0080500a 0000000d"),
    ("cop0", 'cop0', "40087800 0000000d"),
    ("ddr2", 'ddr2', "3c082000 ad090100 0000000d"),          # run from ISCR (not BIOS)
]
LOAD_AT = {"ddr2": 0x60000000}

# name, quirk, load address, words, ON check, OFF check.  A check is a list of REG=VAL, or
# "exc" (standard MIPS CPU exception), "rc!=0" (fault), "rc=0", or b"stdout bytes".
EMU = [
    ("jalr_rd", "jalr-rd", 0x40000000, "3c084000 35080014 241f0000 01004809 00000000 0000000d",
     ["ra=0x40000014", "t1=0"], ["t1=0x40000014", "ra=0"]),
    ("lb_sign", "lb-sign", 0x40000000, "3c085000 24090080 a1090000 34098001 a5090002 810a0000 850b0002 00000000 0000000d",
     ["t2=0x80", "t3=0x8001"], ["t2=0xffffff80", "t3=0xffff8001"]),
    ("unm_load", "unmapped", 0x40000000, "3c087000 24090005 240a0006 ad0a0000 8d090000 00000000 0000000d",
     ["t1=0", "t2=6"], "rc!=0"),
    ("unm_fetch", "unmapped-fetch", 0x40000000, "3c089000 01000008 00000000 0000000d", "rc=0", "rc!=0"),
    ("bios_store", "bios-store", 0x40000000, "3c084000 24091234 ad090100 8d0a0100 00000000 0000000d",
     ["t2=0"], ["t2=0x1234"]),
    ("ddr2", "ddr2", 0x60000000, "3c082000 24090055 ad090100 3c0b1000 8d6a0100 00000000 0000000d",
     ["t2=0"], ["t2=0x55"]),
    ("unaligned", "unaligned", 0x40000000,
     "3c085000 3c091122 35293344 ad090000 240a0007 8d0a0001 240b0099 ad0b0002 8d0c0000 00000000 0000000d",
     ["t2=0", "t4=0x11223344", "t0=0x50000000"], "exc"),
    ("lwlr", "lwlr", 0x40000000, "3c085000 3c091122 35293344 ad090000 240a0077 890a0001 00000000 0000000d",
     ["t2=0", "t0=0x50000000"], ["t2=0x22334477"]),
    ("overflow", "overflow", 0x40000000, "3c047fff 3484ffff 24050001 00855020 0000000d",
     ["t2=0x80000000", "a0=0x7fffffff", "a1=1"], "exc"),
    ("hilo", "hilo", 0x40000000, "24040003 24050005 00850018 00005012 0000000d", ["t2=0"], ["t2=15"]),
    ("syscall", "syscall", 0x40000000, "240a0001 0000000c 240a0002 0000000d", ["t2=2"], "exc"),
    ("movz", "reserved", 0x40000000, "24040007 240a0005 0080500a 0000000d", ["t2=0"], ["t2=7"]),
    ("tgei", "reserved", 0x40000000, "2404ffff 240a0000 04880002 254a0001 254a0002 254a0004 0000000d",
     ["t2=5"], ["t2=7"]),
    ("cop0", "cop0", 0x40000000, "24081234 40887000 400a7000 400b5800 0000000d",
     ["t2=0", "t3=0xffff"], ["t2=0x1234"]),
    ("mmio_lane", "mmio-lane", 0x40000000, "3c088000 24090041 a1090008 0000000d", b"\0", b"A"),
    ("load_use", "load-use", 0x40000000, "3c085000 24090099 ad090000 240a0001 8d0a0000 01405821 0000000d",
     ["t3=1", "t2=0x99"], ["t3=0x99", "t2=0x99"]),
    ("bal_untaken", "bal-untaken", 0x40000000, "241f0000 24040001 04900001 00000000 0000000d",
     ["ra=0"], ["ra=0x40000010"]),
]


def run(path, *opts, at=0x40000000, env=ENV):
    r = subprocess.run([VV, "--no-window", "--quiet-mmio", "--no-random-ddr", "--max-insns", "1000", *opts,
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
    tmp = tempfile.mkdtemp(prefix="vv_quirk_")
    fails = total = 0
    print("detection:")
    for name, want, words in PROGS:
        at = LOAD_AT.get(name, 0x40000000)
        path = os.path.join(tmp, name + ".bin")
        open(path, "wb").write(bytes.fromhex(words.replace(" ", "")))
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
    for name, q, at, words, on, off in EMU:
        path = os.path.join(tmp, "emu_" + name + ".bin")
        open(path, "wb").write(bytes.fromhex(words.replace(" ", "")))
        ok_on = check(path, at, [f"--quirks=+{q}"], on)
        ok_off = check(path, at, [f"--quirks=-{q}"], off)
        total += 1; fails += not (ok_on and ok_off)
        print(f"  {name:12s} {q:14s} on {'ok' if ok_on else 'BAD'}  off {'ok' if ok_off else 'BAD'}  {'PASS' if ok_on and ok_off else 'FAIL'}")
    print("interface:")
    hilo = os.path.join(tmp, "emu_hilo.bin"); jalr = os.path.join(tmp, "emu_jalr_rd.bin")
    cases = [
        ("default = hardware", lambda: check(hilo, 0x40000000, [], ["t2=0"]) and check(jalr, 0x40000000, [], ["t1=0x40000014"])),
        ("--no-quirks", lambda: check(hilo, 0x40000000, ["--no-quirks"], ["t2=15"])),
        ("--quirks=none,hilo", lambda: check(hilo, 0x40000000, ["--quirks=none,hilo"], ["t2=0"])),
        ("--quirks=all", lambda: check(jalr, 0x40000000, ["--quirks=all"], ["ra=0x40000014"])),
        ("env VVIZARD_QUIRKS=-hilo", lambda: run(hilo, "--expect=t2=15", env=dict(ENV, VVIZARD_QUIRKS="-hilo"))[0] == 0),
        ("env then CLI +hilo", lambda: run(hilo, "--quirks=+hilo", "--expect=t2=0", env=dict(ENV, VVIZARD_QUIRKS="-hilo"))[0] == 0),
        ("bad name -> rc 2", lambda: run(hilo, "--quirks=bogus")[0] == 2),
        ("--list-quirks", lambda: subprocess.run([VV, "--list-quirks"], capture_output=True, env=ENV, text=True).stdout.count("\n") == 17),
    ]
    for label, fn in cases:
        ok = fn(); total += 1; fails += not ok
        print(f"  {label:28s} {'PASS' if ok else 'FAIL'}")
    print(f"{'PASS' if not fails else 'FAIL'}: {total - fails}/{total}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
