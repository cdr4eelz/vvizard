#!/usr/bin/env python3
"""Vendored cs150 headers (make test-vendor; docs/VENDORED-HEADERS-PLAN.md).

Hermetic: copies vendor/cs150 and builds throwaway "cs150" trees in a temp directory; never reads or
writes ../../software.
A. scripts/check_vendor.py: identical / changed / partial / absent trees, known-drift handling, hw.inc,
   CRLF, PROVENANCE sha256, printer values, --update.
B. scripts/vendor-cmp.sh through make vendor-cmp: the loud banner (stderr), VENDOR_CHECK=warn|fail|off.
C. Compile-time checks: a drifted vendored header (or types.h) breaks build/cs150_check.o / types_check.o."""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VENDOR = os.path.join(ROOT, "vendor", "cs150")
SCRIPT = os.path.join(ROOT, "scripts", "check_vendor.py")
FILES = ["software/common/types.h", "software/common/mmio_intr_cop0.h", "software/common/mmio_intr_cop0.s.inc",
         "software/proj_app7/shared7.h", "software/proj_app7/shared7.s.inc"]
TMP = tempfile.mkdtemp(prefix="vv-vendor-test-")


def tree(name, edit=None, drop=None, software=True):
    """A fresh fake cs150 root TMP/name with software/<the five files> (optionally edited / one dropped)."""
    root = os.path.join(TMP, name)
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    if software:
        for f in FILES:
            if f == drop:
                os.makedirs(os.path.dirname(os.path.join(root, f)), exist_ok=True); continue
            os.makedirs(os.path.dirname(os.path.join(root, f)), exist_ok=True)
            shutil.copyfile(os.path.join(VENDOR, f), os.path.join(root, f))
    for f, fn in (edit or {}).items():
        p = os.path.join(root, f)
        with open(p, "rb") as h:
            b = h.read()
        with open(p, "wb") as h:
            h.write(fn(b))
    return os.path.join(root, "software")


def vendor_copy(name, edit=None):
    d = os.path.join(TMP, name)
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(VENDOR, d)
    for f, fn in (edit or {}).items():
        p = os.path.join(d, f)
        with open(p, "rb") as h:
            b = h.read()
        with open(p, "wb") as h:
            h.write(fn(b))
    return d


def cv(sw, mode="warn", vendor=VENDOR, hw_inc=os.path.join(HERE, "hw", "hw.inc"), *extra):
    cmd = [sys.executable, SCRIPT, "--mode", mode, "--vendor", vendor, "--upstream", sw, "--build", TMP, *extra]
    if hw_inc:
        cmd += ["--hw-inc", hw_inc]
    r = subprocess.run(cmd, capture_output=True, text=True)
    r.all = r.stdout + r.stderr
    return r


def sub(old, new):
    return lambda b: b.replace(old.encode(), new.encode(), 1)


# ---- A: check_vendor.py ----
def t_identical():
    sw = tree("same")
    w, f = cv(sw), cv(sw, "fail")
    return w.returncode == 0 and f.returncode == 0 and "all 5 vendored files match" in w.all \
        and "known drift K_MAGIC_VERSION shared7.h=0xFEDBEEF3 shared7.s.inc=0xFEDBEEF2" in w.all \
        and "known drift K_MAJOR_VERSION" in w.all and "check-vendor: OK" in w.all and "WARNING" not in w.stderr
def t_byte_flip():
    sw = tree("flip", {FILES[1]: sub("(0x0084u)", "(0x0088u)")})
    w, f = cv(sw), cv(sw, "fail")
    return w.returncode == 0 and f.returncode == 1 and "+#define OW_HALTVAL      (0x0088u)" in w.stdout \
        and "WARNING: VENDORED cs150 HEADERS DIFFER" in w.stderr and "DIFFERS  software/common/mmio_intr_cop0.h" in w.stderr \
        and "OW_HALTVAL: mmio_intr_cop0.h=0x88 mmio_intr_cop0.s.inc=0x84" in w.stdout   # cs150 .h vs .s.inc too
def t_partial():
    sw = tree("partial", drop=FILES[4])
    w, f = cv(sw), cv(sw, "fail")
    return w.returncode == 0 and f.returncode == 1 and "partial" in w.stdout and "MISSING  software/proj_app7/shared7.s.inc" in w.stderr
def t_absent():
    sw = tree("absent", software=False)
    w, f = cv(sw), cv(sw, "fail")
    return w.returncode == 0 and f.returncode == 0 and "skipped upstream compare" in w.stdout and "known drift" in w.stdout
def t_off():
    r = cv(tree("off", {FILES[1]: sub("(0x0084u)", "(0x0088u)")}), "off")
    return r.returncode == 0 and r.stdout.strip() == "check-vendor: skipped (VENDOR_CHECK=off)"
def t_drift_other_pair():   # asm magic changed to something not recorded: the exception does not apply
    v = vendor_copy("v_f0", {FILES[4]: sub("0xFEDBEEF2", "0xFEDBEEF0")})
    sw = tree("none_f0", software=False)
    r = cv(sw, "fail", v, None, )
    return r.returncode == 1 and "does not match known-drift" in r.stdout
def t_drift_fixed():        # shared7.s.inc fixed upstream (F3 / 7): the known-drift entry is stale
    edit = {FILES[4]: lambda b: b.replace(b"0xFEDBEEF2", b"0xFEDBEEF3").replace(b"K_MAJOR_VERSION,    6", b"K_MAJOR_VERSION,    7")}
    sw = tree("fixed", edit)
    w, f = cv(sw), cv(sw, "fail")
    return w.returncode == 0 and f.returncode == 1 and "no longer differs" in w.stdout and "DIFFERS  software/proj_app7/shared7.s.inc" in w.stderr
def t_hw_inc():
    p = os.path.join(TMP, "hw.inc")
    with open(os.path.join(HERE, "hw", "hw.inc")) as h:
        open(p, "w").write(h.read().replace(".equiv HALTVAL,  0x84", ".equiv HALTVAL,  0x88"))
    r = cv(tree("hwinc"), "fail", VENDOR, p)
    return r.returncode == 1 and "HALTVAL = 0x88, vendored OW_HALTVAL = 0x84" in r.stdout
def t_crlf():
    sw = tree("crlf", {FILES[3]: lambda b: b.replace(b"\n", b"\r\n")})
    w, f = cv(sw), cv(sw, "fail")
    return w.returncode == 0 and f.returncode == 1 and "only line endings differ" in w.stdout
def t_provenance():         # a vendored copy edited by hand (not via vendor-update)
    v = vendor_copy("v_edit", {FILES[0]: lambda b: b + b"\n"})
    r = cv(tree("prov", software=False), "fail", v)
    return r.returncode == 1 and "was edited after vendoring" in r.stdout
def t_values():             # printer values on the real vendored pair
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import check_vendor as c
    d = tempfile.mkdtemp(dir=TMP)
    v, cop0 = c.values("gcc", VENDOR, d, "t")
    h, s = v[FILES[3]], v[FILES[4]]
    m = v[FILES[1]]
    return h["SMO_magic2"] == 0x13C and s["SMO_magic2"] == 0x13C and m["IM_UARX"] == 0x400 and m["IM_POSSIBLE"] == 0xFC00 \
        and h["K_TIMER_CYC"] == 50000000 and h["SM_BASE_ADDR"] == s["SM_BASE"] == 0x50000000 \
        and h["SMI_flags_quit"] == s["SMI_flags_quit"] == 0xFFFB and m["II_UARX"] == 0xF800 \
        and cop0[FILES[1]]["COP0_Status"] == cop0[FILES[2]]["COP0_Status"] == "12" and "HALT" not in m
def t_update():
    sw = tree("upd", {FILES[1]: sub("(0x0084u)", "(0x0088u)")})
    v = vendor_copy("v_upd")
    r = subprocess.run([sys.executable, SCRIPT, "--update", "--vendor", v, "--upstream", sw], capture_output=True, text=True)
    prov = open(os.path.join(v, "PROVENANCE")).read()
    after = cv(sw, "fail", v, None)
    missing = subprocess.run([sys.executable, SCRIPT, "--update", "--vendor", v, "--upstream", tree("upd2", drop=FILES[0])],
                             capture_output=True, text=True)
    return r.returncode == 0 and "source_commit: unknown" in prov and "source_dirty: unknown" in prov \
        and open(os.path.join(v, "known-drift")).read() == open(os.path.join(VENDOR, "known-drift")).read() \
        and b"(0x0088u)" in open(os.path.join(v, FILES[1]), "rb").read() and "all 5 vendored files match" in after.stdout \
        and "OW_HALTVAL: mmio_intr_cop0.h=0x88" in after.stdout and missing.returncode == 1


# ---- B: make vendor-cmp (every build) ----
def make_cmp(sw, mode="warn"):
    r = subprocess.run(["make", "-s", "--no-print-directory", "vendor-cmp", f"CS150_SW={sw}", f"VENDOR_CHECK={mode}"],
                       cwd=ROOT, capture_output=True, text=True)
    return r
def t_make_banner():
    sw = tree("mk", {FILES[1]: sub("(0x0084u)", "(0x0088u)")}, drop=FILES[4])
    w, f, o = make_cmp(sw), make_cmp(sw, "fail"), make_cmp(sw, "off")
    ok = make_cmp(tree("mk_ok"))
    gone = make_cmp(tree("mk_gone", software=False))
    return w.returncode == 0 and f.returncode != 0 and o.returncode == 0 and o.stdout + o.stderr == "" \
        and w.stdout == "" and w.stderr.count("#" * 78) == 4 \
        and "WARNING: VENDORED cs150 HEADERS DIFFER FROM THE cs150 ORIGINALS" in w.stderr \
        and "(2 of 5 files" in w.stderr and "DIFFERS  software/common/mmio_intr_cop0.h   (first difference at line 95)" in w.stderr \
        and "MISSING  software/proj_app7/shared7.s.inc" in w.stderr and f"make check-vendor CS150_SW={sw}" in w.stderr \
        and "stopping the build" in f.stderr and ok.returncode == 0 and "all 5 vendored cs150 headers match" in ok.stdout \
        and ok.stderr == "" and gone.returncode == 0 and "no cs150 tree" in gone.stdout


# ---- C: compile-time checks ----
def cc_check(v, src="cs150_check.c"):
    pc = subprocess.run(["pkg-config", "--cflags", "unicorn"], capture_output=True, text=True,
                        env=dict(os.environ, PKG_CONFIG_PATH=os.path.expanduser("~/.local/lib/pkgconfig"))).stdout.split()
    inc = ["-I" + os.path.join(v, "software", "common")]
    if src == "cs150_check.c":
        inc = ["-I" + os.path.join(ROOT, "src"), "-I" + os.path.join(ROOT, "src", "cs150_host"), *inc,
               "-I" + os.path.join(v, "software", "proj_app7"), *pc]
    return subprocess.run(["gcc", "-std=gnu99", "-Wall", "-Wextra", "-fsyntax-only", *inc,
                           os.path.join(ROOT, "src", "cs150_host", src)], capture_output=True, text=True)
def t_compile_ok():
    return cc_check(VENDOR).returncode == 0 and cc_check(VENDOR, "types_check.c").returncode == 0
def t_compile_offset():     # shared7.h moves SMO_flags: VV_SM7_flags no longer matches, nor does the struct
    r = cc_check(vendor_copy("c_off", {FILES[3]: sub("#define SMO_flags             (0x001Au)", "#define SMO_flags             (0x001Cu)")}))
    return r.returncode != 0 and "VV_SM7_flags == SMO_flags" in r.stderr and "SM_DATA.flags offset" in r.stderr
def t_compile_struct():     # a member added to struct SM_DATA without new offsets
    r = cc_check(vendor_copy("c_struct", {FILES[3]: sub("volatile uint32_t magic2;", "volatile uint32_t extra, magic2;")}))
    return r.returncode != 0 and "SM_DATA.magic2 offset" in r.stderr and "sizeof(struct SM_DATA) == SMO_PAST_END" in r.stderr
def t_compile_mmio():       # a new HaltVal address in mmio_intr_cop0.h
    r = cc_check(vendor_copy("c_mmio", {FILES[1]: sub("(0x0084u)", "(0x0088u)")}))
    return r.returncode != 0 and "VV_OW_HALTVAL == OW_HALTVAL" in r.stderr
def t_compile_types():      # types.h with a wrong-width uint16_t
    r = cc_check(vendor_copy("c_types", {FILES[0]: sub("typedef __UINT16_TYPE__     uint16_t;", "typedef __UINT32_TYPE__     uint16_t;")}),
                 "types_check.c")
    return r.returncode != 0 and "types.h 16-bit" in r.stderr


TESTS = [t_identical, t_byte_flip, t_partial, t_absent, t_off, t_drift_other_pair, t_drift_fixed, t_hw_inc, t_crlf,
         t_provenance, t_values, t_update, t_make_banner, t_compile_ok, t_compile_offset, t_compile_struct,
         t_compile_mmio, t_compile_types]


def main():
    fails = 0
    try:
        for t in TESTS:
            try:
                ok = t()
            except Exception as e:          # a crash is a failure, with the reason
                print(f"    {t.__name__}: {type(e).__name__}: {e}"); ok = False
            fails += not ok
            print(f"  {t.__name__[2:]:18s} {'PASS' if ok else 'FAIL'}")
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    print(f"{'PASS' if not fails else 'FAIL'}: {len(TESTS) - fails}/{len(TESTS)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
