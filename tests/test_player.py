#!/usr/bin/env python3
"""proj7 player-control integration test.

proj7 moves the player from its UART-RX ISR (isr7.s): keys u i o / j k l / m , .
(3x3 keypad; 'k' = centre = no move).  Orthogonal keys move 6 px, diagonals 4 px.
We boot GIOS -> proj7 headless with --input-script: each segment 'presses' one key
PRESSES times, one byte every GAP frames, then dumps the displayed frame SETTLE frames
later.  The player ellipse (outline 0xFF6644, fill 0x4466FF) is located by bbox centre
and each segment's displacement is compared with the expected (dx, dy).
"""
import os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SW = os.path.abspath(os.path.join(ROOT, "..", "..", "software"))
VV = os.path.join(ROOT, "vvizard")

PRESSES, GAP, SETTLE, START = 3, 1, 4, 4
# name, key, unit (dx, dy) per press
STEPS = [
    ("idle",       None, (0, 0)),
    ("up",         "i", (0, -6)),
    ("idle",       None, (0, 0)),
    ("down",       ",", (0, 6)),
    ("left",       "j", (-6, 0)),
    ("right",      "l", (6, 0)),
    ("up-left",    "u", (-4, -4)),
    ("idle",       None, (0, 0)),
    ("up-right",   "o", (4, -4)),
    ("down-left",  "m", (-4, 4)),
    ("down-right", ".", (4, 4)),
    ("centre(k)",  "k", (0, 0)),
]
PLAYER = (0xFF6644, 0x4466FF)


def load_ppm(path):
    d = open(path, "rb").read()
    magic, dims, mx, pix = d.split(b"\n", 3)
    w, h = map(int, dims.split())
    assert magic == b"P6" and len(pix) == w * h * 3
    return w, h, pix


def locate(path):
    w, h, pix = load_ppm(path)
    want = {bytes(((c >> 16) & 255, (c >> 8) & 255, c & 255)) for c in PLAYER}
    xs, ys = [], []
    for i in range(0, len(pix), 3):
        if pix[i:i + 3] in want:
            p = i // 3; xs.append(p % w); ys.append(p // w)
    if len(xs) < 20:
        raise AssertionError(f"player not found in {path} ({len(xs)} px)")
    return (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, len(xs)


def main():
    tmp = tempfile.mkdtemp(prefix="vv_player_")
    lines, dumps, f = [], [], START
    lines.append(f"f{f} dump {tmp}/s00.ppm"); dumps.append(f"{tmp}/s00.ppm")
    for n, (name, key, _) in enumerate(STEPS, 1):
        for k in range(PRESSES):
            if key: lines.append(f"f{f + 1 + k * GAP} key {key}")
        f += 1 + (PRESSES - 1) * GAP + SETTLE
        p = f"{tmp}/s{n:02d}.ppm"; lines.append(f"f{f} dump {p}"); dumps.append(p)
    script = os.path.join(tmp, "input.txt")
    open(script, "w").write("\n".join(lines) + "\n")
    max_insns = (f + 4) * 900000      # PixelFeeder frame = 865800 cycles
    cmd = [VV, "--no-window", "--quiet-mmio", "--no-random-ddr",
           "--bios", f"{SW}/gios/gios.inst.bin", "--uart-in", "jal 60000000\\r",
           "--input-script", script, "--max-insns", str(max_insns),
           "--load", f"{SW}/draw/draw.inst.bin@0x50000000",
           "--load", f"{SW}/proj_app7/isr7.bin@0xC0000000",
           "--load", f"{SW}/proj_app7/proj7.inst.bin@0x60000000", "--entry", "0x40000000"]
    t0 = time.time()
    r = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, text=True, env=dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib")))
    emu_t = time.time() - t0
    irq = [l for l in r.stderr.splitlines() if l.startswith("[irq]")]
    print(f"vvizard rc={r.returncode} {emu_t:.2f}s; {irq[0] if irq else 'no [irq] line'}")
    missing = [p for p in dumps if not os.path.exists(p)]
    if r.returncode or missing:
        print(r.stderr[-3000:]); print("FAIL: emulator error or missing dumps", missing); return 1
    pos = [locate(p) for p in dumps]
    print(f"start at ({pos[0][0]:.1f},{pos[0][1]:.1f})")
    fails = 0
    for (name, key, (ux, uy)), a, b in zip(STEPS, pos, pos[1:]):
        dx, dy = b[0] - a[0], b[1] - a[1]
        ex, ey = ux * PRESSES, uy * PRESSES
        ok = abs(dx - ex) <= 1 and abs(dy - ey) <= 1
        fails += not ok
        print(f"  {name:11s} key={key or '-':2s} moved ({dx:+5.1f},{dy:+5.1f}) expect ({ex:+3d},{ey:+3d}) "
              f"-> ({b[0]:.1f},{b[1]:.1f}) {'PASS' if ok else 'FAIL'}")
    print(f"{'PASS' if not fails else 'FAIL'}: {len(STEPS) - fails}/{len(STEPS)} segments")
    if not fails:
        import shutil; shutil.rmtree(tmp)
    else:
        print("frames kept in", tmp)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
