#!/usr/bin/env python3
"""check_vendor.py - vvizard's vendored cs150 headers vs the cs150 originals (docs/VENDORED-HEADERS-PLAN.md).

  make check-vendor   = check_vendor.py --mode $(VENDOR_CHECK) ...   (part of make test)
  make vendor-update  = check_vendor.py --update ...                 (recopy + rewrite PROVENANCE)

Check (exit 1 only under --mode fail, and only when a problem was found):
  1. byte compare vendor/cs150/<f> with <cs150>/<f> for the five files, with a unified diff, a
     HEAD-vs-working-tree annotation and a CRLF note; a loud banner on stderr lists every difference;
  2. .h vs .s.inc VALUES (mmio_intr_cop0, shared7), by compiling a tiny host printer: in the vendored
     snapshot and, when they differ, in the cs150 originals; pairs listed in vendor/cs150/known-drift
     are reported but not counted while their values stay exactly as recorded;
  3. tests/hw/hw.inc literals vs the vendored OW_* / MMIO_BASE.
No cs150 tree: a skip line, exit 0 even under fail.  --mode off: one line, exit 0.
"""
import argparse, datetime, difflib, hashlib, os, re, shutil, subprocess, sys, tempfile

FILES = ["software/common/types.h",
         "software/common/mmio_intr_cop0.h",
         "software/common/mmio_intr_cop0.s.inc",
         "software/proj_app7/shared7.h",
         "software/proj_app7/shared7.s.inc"]
PAIRS = [("software/common/mmio_intr_cop0.h", "software/common/mmio_intr_cop0.s.inc"),
         ("software/proj_app7/shared7.h", "software/proj_app7/shared7.s.inc")]
ALIASES = {"SM_BASE_ADDR": "SM_BASE"}          # header name -> asm name
HW_INC = {"TX_READY": "OW_UATX_READY", "RX_VALID": "OW_UARX_VALID", "TX_DATA": "OW_UATX_DATA",
          "RX_DATA": "OW_UARX_DATA", "PF_FRAME": "OW_PF_FRAME", "GP_FRAME": "OW_GP_FRAME",
          "GP_CODE": "OW_GP_GCODE", "GP_STATE": "OW_GP_STATE", "RANDOM32": "OW_RANDOM32",
          "HALTVAL": "OW_HALTVAL"}
BAR = "#" * 78
HERE = os.path.dirname(os.path.abspath(__file__))
GUARD_DIR = os.path.join(os.path.dirname(HERE), "src", "cs150_host")


class Report:
    def __init__(self):
        self.problems = []                       # (kind, text) that VENDOR_CHECK decides on
    def problem(self, kind, text):
        self.problems.append((kind, text)); print(f"check-vendor: {kind}: {text}")


def rd(p):
    with open(p, "rb") as f:
        return f.read()


def sha(p):
    return hashlib.sha256(rd(p)).hexdigest()


def git(sw, *args):
    try:
        r = subprocess.run(["git", "-C", sw, *args], capture_output=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout if r.returncode == 0 else None


def banner(lines, out=sys.stderr):
    print(BAR, file=out)
    for l in lines:
        print(l if l.startswith("  ") or l.startswith(BAR) else "##   " + l if l else "##", file=out)
    print(BAR, file=out)
    out.flush()


# ---- values: compile and run a host printer ----
OBJ_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?!\()\s+(.+?)\s*(?://.*)?$")
SAFE_BODY = re.compile(r"^[0-9A-Za-z_() +\-<>&|~*]+$")
COP0 = re.compile(r'^"?\$(\d+)"?$')


def header_macros(text):
    out = {}
    for line in text.splitlines():
        m = OBJ_DEFINE.match(line)
        if m:
            out[m.group(1)] = m.group(2).strip()
    return out


def asm_macros(text):
    out = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0]
        m = re.match(r"^\s*\.equiv\s+([A-Za-z_]\w*)\s*,\s*(.+?)\s*$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def printable(name, body):
    return SAFE_BODY.match(body) and not re.search(r"volatile|struct|\*\s*\(", body) and body != ""


def run_printer(cc, workdir, tag, prelude, names, incs=()):
    """Compile prelude + printf of every name; returns {name: 32-bit value}."""
    src = os.path.join(workdir, f"print_{tag}.c")
    exe = os.path.join(workdir, f"print_{tag}")
    with open(src, "w") as f:
        f.write("#include <stdio.h>\n" + prelude + "\nint main(void) {\n")
        for n in names:
            f.write(f'    printf("%s=%llu\\n", "{n}", (unsigned long long)({n}) & 0xFFFFFFFFull);\n')
        f.write("    return 0;\n}\n")
    r = subprocess.run([*cc.split(), "-std=gnu99", "-w", "-I" + GUARD_DIR, *["-I" + i for i in incs], src, "-o", exe], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"printer {tag} did not compile:\n{r.stderr}")
    out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout
    return {k: int(v) for k, v in (l.split("=", 1) for l in out.splitlines())}


def values(cc, root, workdir, tag):
    """{relpath: {name: value}} for the .h and .s.inc files under root, plus {relpath: {COP0_x: N}}."""
    vals, cop0 = {}, {}
    for h, s in PAIRS:
        hm = header_macros(rd(os.path.join(root, h)).decode("latin-1"))
        am = asm_macros(rd(os.path.join(root, s)).decode("latin-1"))
        cop0[h] = {k: COP0.match(v).group(1) for k, v in hm.items() if COP0.match(v)}
        cop0[s] = {k: COP0.match(v).group(1) for k, v in am.items() if COP0.match(v)}
        hn = [k for k, v in hm.items() if k not in cop0[h] and printable(k, v) and not k.endswith("_H_")]
        inc = os.path.abspath(os.path.join(root, h))
        vals[h] = run_printer(cc, workdir, f"{tag}_{os.path.basename(h)}".replace(".", "_"),
                              f'#include "types_guard.h"\n#include "{inc}"\n', hn,
                              [os.path.join(root, "software", "common")])
        defs = "".join(f"#define {k} ({v})\n" for k, v in am.items() if k not in cop0[s])
        vals[s] = run_printer(cc, workdir, f"{tag}_{os.path.basename(s)}".replace(".", "_"), defs,
                              [k for k in am if k not in cop0[s]])
    return vals, cop0


def load_known_drift(path):
    kd = {}
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#", 1)[0].split()
            if len(line) == 3:
                kd[line[0]] = {p.split("=", 1)[0]: int(p.split("=", 1)[1], 0) for p in line[1:]}
    return kd


def compare_pairs(vals, cop0, kd, rep, label, quiet_known=False):
    n = 0
    for h, s in PAIRS:
        hv, sv = vals[h], vals[s]
        common = [(k, k) for k in hv if k in sv] + [(a, b) for a, b in ALIASES.items() if a in hv and b in sv]
        for hk, sk in common:
            n += 1
            a, b = hv[hk], sv[sk]
            known = kd.get(hk)
            if known is not None and known.get(h) is not None:
                if (a, b) == (known.get(h), known.get(s)):
                    if not quiet_known:
                        print(f"check-vendor: {label}known drift {hk} {os.path.basename(h)}=0x{a:X} "
                              f"{os.path.basename(s)}=0x{b:X} (intentional, vendor/cs150/known-drift)")
                    continue
                if a == b:
                    rep.problem("known-drift stale", f"{label}{hk} no longer differs ({os.path.basename(h)} and "
                                f"{os.path.basename(s)} both 0x{a:X}); update vendor/cs150/known-drift, README "
                                f"'Shared memory' and the src/shmem.c stamp warning")
                    continue
                rep.problem("value mismatch", f"{label}{hk}: {os.path.basename(h)}=0x{a:X} {os.path.basename(s)}=0x{b:X} "
                            f"(does not match known-drift 0x{known.get(h):X}/0x{known.get(s):X})")
                continue
            if a != b:
                rep.problem("value mismatch", f"{label}{hk}: {os.path.basename(h)}=0x{a:X} {os.path.basename(s)}"
                            f"{'(' + sk + ')' if sk != hk else ''}=0x{b:X}")
        for k in sorted(set(cop0[h]) & set(cop0[s])):
            n += 1
            if cop0[h][k] != cop0[s][k]:
                rep.problem("value mismatch", f"{label}{k}: {os.path.basename(h)}=${cop0[h][k]} {os.path.basename(s)}=${cop0[s][k]}")
    return n


def check_hw_inc(path, hv, rep):
    t = rd(path).decode("latin-1")
    eq = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"^\.equiv\s+(\w+)\s*,\s*(0x[0-9a-fA-F]+|\d+)", t, re.M)}
    checks = [("IO", hv["MMIO_BASE"] >> 16, "MMIO_BASE >> 16")] + [(k, hv[v], v) for k, v in HW_INC.items()]
    for k, want, what in checks:
        if k not in eq:
            rep.problem("hw.inc", f"{path}: .equiv {k} missing (expected 0x{want:X} = {what})")
        elif eq[k] != want:
            rep.problem("hw.inc", f"{path}: {k} = 0x{eq[k]:X}, vendored {what} = 0x{want:X}")
    return len(checks)


# ---- check ----
def check(a):
    if a.mode == "off":
        print("check-vendor: skipped (VENDOR_CHECK=off)"); return 0
    rep = Report()
    kd = load_known_drift(os.path.join(a.vendor, "known-drift"))
    os.makedirs(a.build, exist_ok=True)
    work = tempfile.mkdtemp(prefix="vendor-check-", dir=a.build)
    try:
        try:
            vv, vc = values(a.cc, a.vendor, work, "vendored")
        except (RuntimeError, OSError, subprocess.CalledProcessError) as e:
            print(f"check-vendor: ERROR: {e}"); return 1
        n = compare_pairs(vv, vc, kd, rep, "vendored: ")
        print(f"check-vendor: vendored .h vs .s.inc: {n} shared names compared")
        n = check_provenance(a.vendor, rep)
        print(f"check-vendor: {n} vendored files match their PROVENANCE sha256")
        if a.hw_inc:
            n = check_hw_inc(a.hw_inc, vv["software/common/mmio_intr_cop0.h"], rep)
            print(f"check-vendor: {a.hw_inc}: {n} literals compared with the vendored OW_* / MMIO_BASE")

        sw = a.upstream
        if not os.path.isdir(sw):
            print(f"check-vendor: skipped upstream compare (no cs150 tree at {sw})")
        else:
            up = os.path.dirname(os.path.abspath(sw))
            diffs, missing = [], []
            for f in FILES:
                u = os.path.join(up, f)
                if not os.path.isfile(u):
                    missing.append(f); rep.problem("partial", f"{f} missing from the cs150 tree ({u})"); continue
                vb, ub = rd(os.path.join(a.vendor, f)), rd(u)
                if vb == ub:
                    continue
                diffs.append(f)
                rep.problem("differs", f"{f}")
                for l in difflib.unified_diff(vb.decode("latin-1").splitlines(True), ub.decode("latin-1").splitlines(True),
                                              f"vendor/cs150/{f}", u):
                    sys.stdout.write(l if l.endswith("\n") else l + "\n")
                if vb.replace(b"\r", b"") == ub.replace(b"\r", b""):
                    print(f"check-vendor:   {f}: only line endings differ (CR bytes)")
                head = git(sw, "show", f"HEAD:{f}")
                if head is not None:
                    print(f"check-vendor:   {f}: " + ("cs150 HEAD still matches the vendored copy; the cs150 working tree "
                          "has uncommitted edits" if head == vb else "cs150 HEAD differs from the vendored copy too"))
            if not diffs and not missing:
                print(f"check-vendor: all {len(FILES)} vendored files match {sw}")
                cur = git(sw, "rev-parse", "HEAD")
                prov = provenance_field(a.vendor, "source_commit")
                if cur and prov and cur.decode().strip() != prov:
                    print(f"check-vendor: note: same bytes, but PROVENANCE records cs150 {prov[:12]} and HEAD is "
                          f"{cur.decode().strip()[:12]} (make vendor-update refreshes the record)")
            elif not missing:
                try:
                    uv, uc = values(a.cc, up, work, "upstream")
                    n = compare_pairs(uv, uc, kd, rep, "cs150: ", quiet_known=True)
                    print(f"check-vendor: cs150 .h vs .s.inc: {n} shared names compared")
                except (RuntimeError, OSError, subprocess.CalledProcessError) as e:
                    rep.problem("printer", f"cs150 headers could not be evaluated: {e}")
            if diffs or missing:
                banner(["", "WARNING: VENDORED cs150 HEADERS DIFFER FROM THE cs150 ORIGINALS",
                        f"({len(diffs) + len(missing)} of {len(FILES)} files; vvizard is built against {a.vendor}/)", "", BAR]
                       + [f"  DIFFERS  {f}" for f in diffs] + [f"  MISSING  {f}" for f in missing]
                       + [BAR, f"vendored from cs150 {(provenance_field(a.vendor, 'source_commit') or 'unknown')[:12]}"
                          f" ({a.vendor}/PROVENANCE)", "the diff is printed above (stdout)",
                          "Take cs150's: make vendor-update, then make test and commit vendor/",
                          "Strict / quiet: make VENDOR_CHECK=fail  |  make VENDOR_CHECK=off"])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if not rep.problems:
        print("check-vendor: OK"); return 0
    print(f"check-vendor: {len(rep.problems)} problem(s) (VENDOR_CHECK={a.mode})")
    return 1 if a.mode == "fail" else 0


def check_provenance(vendor, rep):
    p = os.path.join(vendor, "PROVENANCE")
    if not os.path.exists(p):
        rep.problem("provenance", f"{p} missing (make vendor-update writes it)"); return 0
    rec = dict(re.findall(r"^  (\S+) sha256:([0-9a-f]{64})$", open(p).read(), re.M))
    ok = 0
    for f in FILES:
        if f not in rec:
            rep.problem("provenance", f"{f} not recorded in {p}")
        elif sha(os.path.join(vendor, f)) != rec[f]:
            rep.problem("provenance", f"vendor/cs150/{f} was edited after vendoring (sha256 differs from PROVENANCE); "
                        "vendored files must stay byte copies: make vendor-update")
        else:
            ok += 1
    return ok


def provenance_field(vendor, key):
    p = os.path.join(vendor, "PROVENANCE")
    if not os.path.exists(p):
        return None
    m = re.search(rf"^{key}: (\S+)", open(p).read(), re.M)
    return m.group(1) if m else None


# ---- update ----
def update(a):
    sw = a.upstream
    up = os.path.dirname(os.path.abspath(sw))
    miss = [f for f in FILES if not os.path.isfile(os.path.join(up, f))]
    if miss:
        print("vendor-update: missing in the cs150 tree, nothing copied: " + ", ".join(miss)); return 1
    rels = [os.path.relpath(os.path.join(up, f), sw) for f in FILES]
    commit = git(sw, "rev-parse", "HEAD")
    date = git(sw, "log", "-1", "--format=%cI")
    st = git(sw, "status", "--porcelain", "--", *rels)
    tree = git(sw, "status", "--porcelain")
    dirty = "unknown" if st is None else ("yes" if st.strip() else "no")
    tree_dirty = "unknown" if tree is None else (f"yes ({len(tree.decode().splitlines())} paths)" if tree.strip() else "no")
    for f in FILES:
        os.makedirs(os.path.dirname(os.path.join(a.vendor, f)), exist_ok=True)
        shutil.copyfile(os.path.join(up, f), os.path.join(a.vendor, f))
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    with open(os.path.join(a.vendor, "PROVENANCE"), "w") as p:
        p.write("# Written by make vendor-update (scripts/check_vendor.py --update); do not edit by hand.\n")
        p.write(f"source_root: {a.upstream}\n")
        p.write(f"source_commit: {commit.decode().strip() if commit else 'unknown'}\n")
        p.write(f"source_commit_date: {date.decode().strip() if date else 'unknown'}\n")
        p.write(f"source_dirty: {dirty}\n")
        p.write(f"source_tree_dirty: {tree_dirty}\n")
        p.write(f"vendor_date: {now}\n")
        p.write("files:\n")
        for f in FILES:
            p.write(f"  {f} sha256:{sha(os.path.join(a.vendor, f))}\n")
    print(f"vendor-update: copied {len(FILES)} files from {sw}" + (" (a DIRTY tree: those files have uncommitted edits)"
          if dirty == "yes" else ""))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", default="warn", choices=["warn", "fail", "off"])
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--vendor", default="vendor/cs150")
    ap.add_argument("--upstream", default="../../software")
    ap.add_argument("--hw-inc", default=None)
    ap.add_argument("--build", default="build")
    a = ap.parse_args()
    return update(a) if a.update else check(a)


if __name__ == "__main__":
    sys.exit(main())
