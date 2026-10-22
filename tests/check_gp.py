#!/usr/bin/env python3
"""Check the PPM produced by the gplaunch/gpcmds test (one probe per GPU command)."""
import sys
d = open(sys.argv[1], 'rb').read(); h = d.index(b'255\n') + 4
px = lambda x, y: tuple(d[h + 3 * (y * 800 + x): h + 3 * (y * 800 + x) + 3])
probes = [  # (x, y, expected rgb, what)
    (5, 300, (0, 0, 0x22), "FILL background"),
    (100, 120, (255, 0, 0), "RECT left edge"), (199, 120, (255, 0, 0), "RECT right edge"),
    (150, 100, (255, 0, 0), "RECT top row"), (150, 149, (255, 0, 0), "RECT bottom row"),
    (150, 120, (0, 255, 0), "RECT fill (fill[31:24]!=0)"), (99, 120, (0, 0, 0x22), "RECT outside"),
    (300, 120, (255, 255, 255), "RECT2 edge"), (320, 120, (0, 0, 0x22), "RECT2 transparent interior"),
    (0, 0, (255, 255, 0), "LINE start"), (799, 599, (255, 255, 0), "LINE end"), (400, 300, (255, 255, 0), "LINE mid"),
    (450, 400, (0, 255, 255), "ELIP left edge"), (550, 400, (0, 255, 255), "ELIP right edge"),
    (500, 370, (0, 255, 255), "ELIP top"), (500, 430, (0, 255, 255), "ELIP bottom"),
    (520, 410, (255, 0, 255), "ELIP fill"), (449, 400, (0, 0, 0x22), "ELIP outside"),
]
bad = 0
for x, y, want, what in probes:
    got = px(x, y); ok = got == want; bad += not ok
    print(f"{'PASS' if ok else 'FAIL'} {what:30s} ({x},{y}) got {got} want {want}")
sys.exit(1 if bad else 0)
