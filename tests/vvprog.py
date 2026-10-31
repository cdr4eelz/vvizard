"""Test programs for the vvizard tests: prog("loop") / prog("quirk/det_lb_neg") -> (bin path, load addr).

The Makefile builds tests/NAME.s into build/tests/NAME.bin (make tests).  When that .bin is missing
or older than its .s (no MIPS toolchain), the committed fallback tests/NAME.hex is converted instead.
The load address is the "# vv-load:" line of the .s."""
import os, re, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
_TMP = None


def load_addr(name):
    m = re.search(r"^# vv-load: *(\S+)", open(os.path.join(HERE, name + ".s")).read(), re.M)
    return int(m.group(1), 0)


def prog(name):
    global _TMP
    src = os.path.join(HERE, name + ".s")
    b = os.path.join(ROOT, "build", "tests", name + ".bin")
    if os.path.exists(b) and os.path.getmtime(b) >= os.path.getmtime(src):
        return b, load_addr(name)
    _TMP = _TMP or tempfile.mkdtemp(prefix="vv_prog_")
    p = os.path.join(_TMP, name.replace("/", "_") + ".bin")
    words = "".join(open(os.path.join(HERE, name + ".hex")).read().split())
    open(p, "wb").write(bytes.fromhex(words))
    return p, load_addr(name)
