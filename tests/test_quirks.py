#!/usr/bin/env python3
"""--halt-on-quirk unit test: one tiny program per category (loaded at 0x40000000, ends in
`break`).  Each 'bad' program must stop with exit 4 and '[quirk] NAME at'; each '_ok'/'_pos'
program must run clean (exit 0, no quirk).  Hex was assembled with mips-linux-gnu-as -mips1 -EB."""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VV = os.path.join(os.path.dirname(HERE), "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
# name, expected quirk (None = must not trip), big-endian words
PROGS = [
    ("lb_neg", None, "3c085000 24090080 a1090000 00000000 810a0000 00000000 00000000 0000000d"),
    ("lb_pos", None, "3c085000 2409007f a1090000 00000000 810a0000 00000000 24097fff a5090002 850b0002 00000000 910c0000 00000000 0000000d"),
    ("lh_neg", None, "3c085000 34098001 a5090002 850b0002 00000000 0000000d"),
    ("jalr_rd", 'jalr-rd', "3c084000 35080010 01004809 00000000 00000000 00000000 0000000d"),
    ("jalr_ok", None, "3c084000 35080010 0100f809 00000000 00000000 00000000 0000000d"),
    ("unm_load", 'unmapped', "3c087000 8d090000 00000000 0000000d"),
    ("unm_store", 'unmapped', "ac090000 0000000d"),
    ("unm_fetch", 'unmapped', "3c089000 01000008 00000000 0000000d"),
    ("mmio_fetch", 'unmapped', "3c088000 01000008 00000000 0000000d"),
    ("bios_store", 'bios-store', "3c084000 ad080100 0000000d"),
    ("unaligned", 'unaligned', "3c085000 8d090002 00000000 0000000d"),
    ("lwl", 'lwlr', "3c085000 89090001 00000000 0000000d"),
    ("overflow", 'overflow', "3c087fff 3508ffff 21090001 0000000d"),
    ("add_ok", None, "24080005 01084820 01084822 250afff7 0000000d"),
    ("hilo", 'hilo', "24080003 01080018 00004812 0000000d"),
    ("syscall", 'syscall', "0000000c 0000000d"),
    ("uart_lane", 'uart-lane', "3c088000 24090041 a1090008 0000000d"),
    ("uart_ok", None, "3c088000 24090041 a109000b ad090008 0000000d"),
    ("load_use", 'load-use', "3c085000 8d090000 01295021 0000000d"),
    ("load_use_ok", None, "3c085000 8d090000 00000000 01295021 0000000d"),
]


def run(path, *opts):
    r = subprocess.run([VV, "--no-window", "--quiet-mmio", "--no-random-ddr", "--max-insns", "1000", *opts,
                        "--load", f"{path}@0x40000000", "--entry", "0x40000000"],
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=ENV)
    return r.returncode, re.findall(r"^\[quirk\] ([a-z-]+) at", r.stderr, re.M)


def main():
    tmp = tempfile.mkdtemp(prefix="vv_quirk_")
    fails = 0
    for name, want, words in PROGS:
        path = os.path.join(tmp, name + ".bin")
        open(path, "wb").write(bytes.fromhex(words.replace(" ", "")))
        rc, hits = run(path, "--halt-on-quirk")
        ok = (rc == 4 and hits == [want]) if want else (rc == 0 and not hits)
        rc_off, hits_off = run(path)                       # option off: never a [quirk] line
        ok = ok and not hits_off
        if want:                                           # excluding the category: no halt
            rc_x, hits_x = run(path, f"--halt-on-quirk=all,-{want}")
            ok = ok and rc_x != 4 and want not in hits_x
        fails += not ok
        print(f"  {name:12s} expect {want or '-':10s} got rc={rc} {hits}  off: rc={rc_off} {'PASS' if ok else 'FAIL'}")
    print(f"{'PASS' if not fails else 'FAIL'}: {len(PROGS) - fails}/{len(PROGS)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
