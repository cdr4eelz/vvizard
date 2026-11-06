#!/usr/bin/env python3
"""proj7 shared memory, isr7 jump table and HALT() tests (make test-sm).

A. --dump-sm decode (src/shmem.c) of synthetic structures: proj6 layout (tests/hw/sm_proj6.s), a proj7
   structure whose magic2 was never written (tests/hw/sm_magic2.s), and none at all.
B. The real software/proj_app7/isr7.bin at 0xC0000000: tests/hw/sm_entry.s calls jump-table slots 0-3
   (16 bytes apart).  Slot 0 (DO_INIT_VERSION) stamps the structure with shared7.s.inc's constants;
   while they differ from shared7.h (today on purpose: 0xFEDBEEF2 / 6) vvizard must still decode the
   proj7 layout and warn about the stamp, also without --dump-sm.
C. The proj7 player (default program: GIOS + bench + isr7 + proj7) after keys: shared7.h stamp and
   fields; 'q' sets the quit flag and proj7 calls HALT(1): HaltVal 1, GIOS restarts and (if this
   GIOS prints it) shows "HALTVAL: 00000001"; with --exit-on-halt the run ends at that store."""
import os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import optional_deps
from pathlib import Path as _Path
from vvprog import prog

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VV = os.path.join(ROOT, "vvizard")
SW = os.path.abspath(os.path.join(ROOT, "..", "..", "software"))
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None); ENV.pop("VVIZARD_EXIT_ON_HALT", None)
BASE = ["--sync", "--no-window", "--frame-clock=emulated", "--quiet-mmio", "--no-random-ddr", "--no-halt-on-quirk", "--color=never"]


def h7_stamp():
    """K_MAGIC_VERSION / K_MAJOR_VERSION from vvizard's vendored shared7.h (= src/shmem.h VV_SM7_*, checked at
    compile time by src/cs150_host/cs150_check.c)."""
    t = open(os.path.join(ROOT, "vendor", "cs150", "software", "proj_app7", "shared7.h")).read()
    g = lambda k: int(re.search(rf"^#define\s+{k}\s+\(?(\w+?)u?\)?\s*$", t, re.M).group(1), 0)
    return g("K_MAGIC_VERSION"), g("K_MAJOR_VERSION")


H7 = h7_stamp()             # (0xFEDBEEF3, 7) today


def run(args, script=None):
    r = subprocess.run([VV, *BASE, *args], stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
    r.err = r.stderr.decode(errors="replace"); r.out = r.stdout.decode(errors="replace")
    return r


def hwprog(name, *extra):
    return run(["--no-bios", "--max-insns", "100000", "--load", f"{prog('hw/' + name)[0]}@0x40000000",
                "--entry", "0x40000000", *extra])


def sm_lines(r):
    return [l for l in r.err.splitlines() if l.startswith("[sm]")]


def s_inc_stamp():
    """K_MAGIC_VERSION / K_MAJOR_VERSION from software/proj_app7/shared7.s.inc (what isr7 stamps)."""
    t = open(os.path.join(SW, "proj_app7", "shared7.s.inc")).read()
    g = lambda k: int(re.search(rf"^\.equiv\s+{k},\s*(\S+)", t, re.M).group(1), 0)
    return g("K_MAGIC_VERSION"), g("K_MAJOR_VERSION")


# ---- A: decoder ----
def t_proj6():
    r = hwprog("sm_proj6", "--dump-sm")
    s = "\n".join(sm_lines(r))
    return r.returncode == 0 and "[sm] proj6 shared memory at 0x50000000 (shared6.h layout, 0x134 bytes)" in s \
        and "magic 0xfedbeef2" in s and "flags 0x0001 (pause 1 notimer 0)" in s and "clock 0x0123 (01:23)" in s \
        and "player x 77 y -5" in s and "buff size 256 head 3 tail 1" in s and "WARNING" not in s
def t_magic2():
    r = hwprog("sm_magic2", "--dump-sm")
    q = hwprog("sm_magic2")                      # warnings also without --dump-sm, only those
    s = "\n".join(sm_lines(r))
    return r.returncode == 0 and "[sm] proj7 shared memory" in s and "magic1 0xfedbeef3  majorvers 7  magic2 0x00000000" in s \
        and "player x 12 y 34  bouncers 5/9" in s and re.search(r"WARNING .*magic2 \(\+0x13c\) 0x00000000 != magic1", s) \
        and "stamp" not in s and q.returncode == 0 and [l for l in sm_lines(q)] == [l for l in sm_lines(r) if "WARNING" in l]
def t_none():
    r = hwprog("uart_tx", "--dump-sm")
    q = hwprog("uart_tx")
    return r.returncode == 0 and sm_lines(r) == ["[sm] no proj6/proj7 shared memory at 0x50000000 (magic 0x00000000)"] \
        and not sm_lines(q)


# ---- B: isr7 jump table + the shared7.s.inc stamp ----
def t_isr7_entry():
    isr7 = os.path.join(SW, "proj_app7", "isr7.bin")
    args = ["--load", f"{isr7}@0xC0000000", "--expect", "s5=0", "--expect", "s3=3"]
    r = hwprog("sm_entry", "--dump-sm", *args)
    q = hwprog("sm_entry", *args)
    magic, major = s_inc_stamp()
    s = "\n".join(sm_lines(r))
    ok = r.returncode == 0 and "[sm] proj7 shared memory" in s \
        and f"magic1 0x{magic:08x}  majorvers {major}  magic2 0x{magic:08x}" in s and "buff size 256" in s
    warn = [l for l in sm_lines(q) if "WARNING" in l]
    if (magic, major) == H7:                     # shared7.s.inc agrees with shared7.h: nothing to report
        return ok and "WARNING" not in s and not warn
    print(f"    (shared7.s.inc stamps 0x{magic:08x} / {major}, shared7.h 0x{H7[0]:08x} / {H7[1]}: expect the stamp warning)")
    stale = "stale K_MAGIC_VERSION/K_MAJOR_VERSION" if (magic, major) == (0xFEDBEEF2, 6) else "shared7.h says"
    return ok and stale in s and q.returncode == 0 and len(warn) == 1 and stale in warn[0]


# ---- C: the proj7 player ----
def player(keys, *extra, frames=40):
    import tempfile
    sc = os.path.join(tempfile.mkdtemp(prefix="vv_sm_"), "keys.txt")
    open(sc, "w").write("".join(f"f{f} key {k}\n" for f, k in keys))
    return run(["--input-script", sc, "--max-insns", str(frames * 900000), *extra])
def t_player_sm():          # d: notimer, i x2: player_y 300 -> 288, space: pause (last: script fN counts
    # PF_FRAME writes and a paused proj7 stops writing PF_FRAME)
    r = player([(10, "d"), (11, "i"), (12, "i"), (13, "\\s")], "--dump-sm")
    s = "\n".join(sm_lines(r))
    return r.returncode == 0 and "[sm] proj7 shared memory at 0x50000000 (shared7.h layout, 0x140 bytes)" in s \
        and f"magic1 0x{H7[0]:08x}  majorvers {H7[1]}  magic2 0x{H7[0]:08x}" in s \
        and "flags 0x0003 (pause 1 notimer 1 quit 0)" in s and "player x 400 y 288  bouncers 1/127" in s \
        and "buff size 256" in s and "WARNING" not in r.err
def gios_prints_haltval():
    return b"HALTVAL: " in open(os.path.join(SW, "gios", "gios.inst.bin"), "rb").read() + \
        open(os.path.join(SW, "gios", "gios.elf"), "rb").read()
def t_quit():               # q: quit flag, proj7 HALT(1): HaltVal = 1, jr 0x40000000 -> GIOS boots again
    r = player([(10, "q")], "--dump-sm", frames=30)
    m = re.search(r"^\[halt\] HaltVal = 0x00000001 \(1\) at pc=(0x6[0-9a-f]{7})$", r.err, re.M)
    banners = r.out.count("[Golt45.")
    ok = r.returncode == 0 and m and banners == 2 and "(pause 0 notimer 0 quit 1)" in r.err \
        and "last stored at pc=" in r.err and "--exit-on-halt" not in r.err
    if gios_prints_haltval():
        ok = ok and re.search(r"\[Golt45\.[^\]]*\]\s*HALTVAL: 00000001", r.out.split("PROJ-7", 1)[-1])
    return bool(ok)
def t_quit_exit_on_halt():  # the same with --exit-on-halt: stops at proj7's HALT() store, status 0
    r = player([(10, "q")], "--exit-on-halt", frames=30)
    return r.returncode == 0 and "-> stop (--exit-on-halt)" in r.err and "run ended by --exit-on-halt" in r.err \
        and r.out.count("[Golt45.") == 1 and "HALTVAL:" not in r.out


TESTS = [t_proj6, t_magic2, t_none, t_isr7_entry, t_player_sm, t_quit, t_quit_exit_on_halt]


SW_DEPENDENT = {t_isr7_entry, t_player_sm, t_quit, t_quit_exit_on_halt}


def main():
    if not os.path.isfile(VV):
        print(f"FAIL: missing vvizard binary at {VV}; run make first", file=sys.stderr)
        return 1
    gaps = optional_deps.software_tree_gaps(_Path(SW))
    fails = skipped = ran = 0
    for t in TESTS:
        if t in SW_DEPENDENT and gaps:
            print(f"  {t.__name__[2:]:18s} SKIP missing {gaps[0]}")
            skipped += 1
            continue
        ok = t()
        fails += not ok
        ran += 1
        print(f"  {t.__name__[2:]:18s} {'PASS' if ok else 'FAIL'}")
    if skipped and ran == 0 and fails == 0:
        return optional_deps.print_skip(
            f"cs150 software tree incomplete at {SW} (missing {', '.join(gaps)})")
    print(f"{'PASS' if not fails else 'FAIL'}: {ran - fails}/{ran} ran, skipped={skipped}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
