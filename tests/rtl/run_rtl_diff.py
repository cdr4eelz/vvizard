#!/usr/bin/env python3
"""Tri-way differential: Unicorn vs fast vs Icarus (user CPUMIPS via CoreHarness).

Does not edit cs150/Verilog. Requires CS150_ROOT (tree with hardware/sim-icarus/tb/CoreHarness.v
and hardware/src/cpu/CPUMIPS.v), iverilog/vvp, and preferably mips-linux-gnu-as/objcopy.

Usage:
  python3 tests/rtl/run_rtl_diff.py --cs150-root /path/to/cs150
  make test-cpu-rtl CS150_ROOT=/path/to/cs150
"""
from __future__ import annotations

import argparse, hashlib, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
VV = ROOT / "vvizard"
CASES = HERE / "cases"
TB = HERE / "CPU_sig_tb.v"
ISR_HEX = HERE / "isr_nop.hex"
ENV = dict(os.environ, LD_LIBRARY_PATH=os.environ.get("LD_LIBRARY_PATH", "") + ":" + os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)

SIG_I = re.compile(r"^SIG i=(\d+) act=([0-9a-fA-F]+) exp=([0-9a-fA-F]+)\s*$")
SIG_N = re.compile(r"^SIG n=(\d+)\s*$")
DUMP = re.compile(r"^\[dump-be32\] a=0x([0-9a-fA-F]+) v=0x([0-9a-fA-F]+) ok=(\d+)\s*$")


def die(msg: str, code: int = 2) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(code)


def _looks_like_cs150(p: Path) -> bool:
    return (p / "hardware/sim-icarus/tb/CoreHarness.v").is_file() and (
        p / "hardware/src/cpu/CPUMIPS.v"
    ).is_file()


def discovery_candidates(root: Path | None = None) -> list[Path]:
    """Ordered discovery paths when CS150_ROOT/--cs150-root unset.

    Prefer live user trees ($HOME/shared/cs150) and named auth extracts before
    unrelated older snapshots such as cs150-1245. Print rtl_revision(path);
    do not infer freshness from directory names alone.
    """
    root = root or ROOT
    cands: list[Path] = []
    home = os.environ.get("HOME")
    if home:
        cands.append(Path(home) / "shared/cs150")
    for rel in (
        "../cs150-auth-0ff61b6",
        "../../cs150-auth-0ff61b6",
        "cs150-auth-0ff61b6",
        "../cs150",
        "../../cs150",
        "../../../cs150",
        "cs150",
        "../cs150-1245",
        "../../cs150-1245",
        "cs150-1245",
    ):
        cands.append(root / rel)
    out: list[Path] = []
    seen: set[Path] = set()
    for pth in cands:
        try:
            rp = pth.expanduser().resolve()
        except Exception:
            continue
        if rp in seen:
            continue
        seen.add(rp)
        out.append(rp)
    return out


def resolve_cs150(explicit: str | None) -> Path:
    # Explicit flag or CS150_ROOT env: require that path (no silent fallback).
    forced = explicit or os.environ.get("CS150_ROOT")
    if forced:
        pth = Path(forced).expanduser().resolve()
        if not _looks_like_cs150(pth):
            die(
                f"CS150_ROOT={pth} is not a usable cs150 tree "
                "(need hardware/sim-icarus/tb/CoreHarness.v and hardware/src/cpu/CPUMIPS.v)"
            )
        return pth
    for pth in discovery_candidates():
        if _looks_like_cs150(pth):
            return pth
    die(
        "CS150_ROOT not found. Set --cs150-root or env CS150_ROOT to a cs150 tree containing "
        "hardware/sim-icarus/tb/CoreHarness.v and hardware/src/cpu/CPUMIPS.v"
    )


def tool_versions() -> dict:
    out = {}
    for name, args in (
        ("iverilog", ["iverilog", "-V"]),
        ("vvp", ["vvp", "-V"]),
        ("mips-as", ["mips-linux-gnu-as", "--version"]),
    ):
        try:
            r = subprocess.run(args, capture_output=True, text=True, timeout=10)
            line = (r.stdout or r.stderr).splitlines()[0] if (r.stdout or r.stderr) else f"rc={r.returncode}"
            out[name] = line.strip()
        except FileNotFoundError:
            out[name] = "MISSING"
        except Exception as e:
            out[name] = f"error:{e}"
    return out


def rtl_revision(cs150: Path) -> str:
    auth = cs150 / ".AUTH_REV"
    if auth.is_file():
        rev = auth.read_text().strip().splitlines()[0].strip()
        if rev:
            return rev
    try:
        r = subprocess.run(["git", "-C", str(cs150), "rev-parse", "HEAD"], capture_output=True, text=True, timeout=10)
        if r.returncode == 0:
            return r.stdout.strip()
    except Exception:
        pass
    h = hashlib.sha256()
    for rel in ("hardware/src/cpu/CPUMIPS.v", "hardware/sim-icarus/tb/CoreHarness.v"):
        h.update((cs150 / rel).read_bytes())
    return "sha256:" + h.hexdigest()[:16]


def need_cmd(cmd: str) -> None:
    if shutil.which(cmd.split()[0] if " " not in cmd else cmd) is None and shutil.which(cmd) is None:
        die(f"required tool not on PATH: {cmd}", 2)


def assemble(case_s: Path, work: Path) -> tuple[Path, Path]:
    """Return (bin_path for vvizard, hex_path for $readmemh)."""
    need_cmd("mips-linux-gnu-as")
    need_cmd("mips-linux-gnu-objcopy")
    obj = work / (case_s.stem + ".o")
    raw = work / (case_s.stem + ".raw")
    bil = work / (case_s.stem + ".bin")
    hx = work / (case_s.stem + ".hex")
    # -mips1 matches MIPS-I load-delay software model used by CoreHarness programs
    isa = "-mips1"
    for line in case_s.read_text().splitlines():
        if line.startswith("# vv-isa:"):
            isa = line.split(":", 1)[1].strip()
            break
    case_s = case_s.resolve()
    # Include path for ../../check.inc from cases/
    r = subprocess.run(
        ["mips-linux-gnu-as", isa, "-O0", "-I", str(HERE.parent), "-I", str(case_s.parent),
         "-o", str(obj), str(case_s)],
        capture_output=True, text=True,
    )
    if r.returncode:
        die(f"assemble {case_s.name} failed:\n{r.stderr}")
    subprocess.check_call(["mips-linux-gnu-objcopy", "-O", "binary", str(obj), str(raw)])
    data = raw.read_bytes()
    # strip ELF? objcopy -O binary from .o may include only .text if we use -j .text
    # Prefer explicit .text extract:
    subprocess.check_call(["mips-linux-gnu-objcopy", "-O", "binary", "-j", ".text", str(obj), str(bil)])
    data = bil.read_bytes()
    if len(data) % 4:
        die(f"{case_s.name}: text size {len(data)} not multiple of 4")
    words = []
    for i in range(0, len(data), 4):
        w = int.from_bytes(data[i : i + 4], "big")
        words.append(f"{w:08x}")
    hx.write_text("\n".join(words) + "\n")
    return bil, hx


def run_rtl(cs150: Path, hex_path: Path, work: Path, timeout_s: float, force_fail: bool = False, max_cycles: int = 50000) -> tuple[list[tuple[int, int]], str, int]:
    need_cmd("iverilog")
    need_cmd("vvp")
    src = cs150 / "hardware" / "src"
    tb_core = cs150 / "hardware" / "sim-icarus" / "tb" / "CoreHarness.v"
    ydirs = [src, src / "cpu", src / "innerds", src / "base_util", src / "base_io", src / "base_mem"]
    yflags = []
    for d in ydirs:
        if d.is_dir():
            yflags += ["-y", str(d)]
    vvp = work / "case.vvp"
    defs = [f'-DMAIN_HEX="{hex_path}"', f'-DISR_HEX="{ISR_HEX}"', f"-DMAX_CYCLES={max_cycles}", "-DNSTALL=4"]
    if force_fail:
        defs.append("-DFORCE_FAIL")
    cmd = ["iverilog", "-g2005", "-grelative-include", "-Wall", "-Wno-timescale", "-s", "CPU_sig_tb", *defs, *yflags, "-o", str(vvp), str(TB), str(tb_core)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if r.returncode:
        die(f"iverilog failed:\n{r.stderr[-2000:]}\n{r.stdout[-1000:]}")
    # run from work so relative paths in hex resolve; hex is absolute so ok
    rr = subprocess.run(["timeout", f"{timeout_s:.0f}", "vvp", str(vvp)], capture_output=True, text=True, cwd=str(work), timeout=timeout_s + 5)
    out = (rr.stdout or "") + (rr.stderr or "")
    if rr.returncode == 124 or "TIMEOUT:" in out:
        return [], out, 124
    pairs = []
    n = None
    for line in out.splitlines():
        m = SIG_N.match(line.strip())
        if m:
            n = int(m.group(1))
        m = SIG_I.match(line.strip())
        if m:
            pairs.append((int(m.group(2), 16), int(m.group(3), 16)))
    if n is not None and len(pairs) != n:
        # still return what we have; caller may fail
        pass
    return pairs, out, rr.returncode


def run_emu(cpu: str, bil: Path, n_pairs: int, timeout_s: float) -> tuple[list[tuple[int, int]], str, int]:
    # Dump from SM base through RESULTS table. RESULTS is at 0x50000400 (word index 0x100).
    # words = 0x100 + 2*n_pairs + padding
    count_words = 0x100 + 2 * max(n_pairs, 1) + 4
    args = [
        str(VV), "--sync", "--no-window", "--frame-clock=emulated", "--quiet-mmio",
        "--no-random-ddr", "--no-bios", "--no-halt-on-quirk", "--max-insns", "200000",
        f"--cpu={cpu}",
        "--quiet-quirk=dcache-stale,tx-busy,rx-overrun,irq-storm,undef-reg,gp-busy",
        "--load", f"{bil}@0x60000000", "--entry", "0x60000000",
        f"--dump-be32=0x50000000:{count_words}",
    ]
    r = subprocess.run(args, capture_output=True, env=ENV, timeout=timeout_s)
    err = r.stderr.decode(errors="replace")
    words = {}
    for line in err.splitlines():
        m = DUMP.match(line.strip())
        if m:
            words[int(m.group(1), 16)] = (int(m.group(2), 16), int(m.group(3)))

    def need(addr: int) -> int:
        if addr not in words:
            raise ValueError(f"missing dump-be32 for a=0x{addr:08x}")
        v, okflag = words[addr]
        if not okflag:
            raise ValueError(f"dump-be32 read failure a=0x{addr:08x} (ok=0)")
        return v

    try:
        magic = need(0x50000000)
        count = need(0x50000004)
    except ValueError as e:
        return [], err + f"\n({e})", r.returncode or 1
    pairs = []
    if magic != 0x600DC0DE:
        return [], err + f"\n(missing DONE_MAGIC got 0x{magic:08x})", r.returncode or 1
    base = 0x50000400
    try:
        for i in range(count):
            act = need(base + 8 * i)
            exp = need(base + 8 * i + 4)
            pairs.append((act, exp))
    except ValueError as e:
        return [], err + f"\n({e})", r.returncode or 1
    return pairs, err, r.returncode


def compare_signatures(name: str, rtl, uni, fast) -> tuple[bool, str]:
    """Return (ok, detail). Requires equal length and act==exp per backend and equal acts across backends."""
    lines = []
    ok = True
    if not rtl or not uni or not fast:
        return False, f"empty signature rtl={len(rtl)} uni={len(uni)} fast={len(fast)}"
    if not (len(rtl) == len(uni) == len(fast)):
        return False, f"length mismatch rtl={len(rtl)} uni={len(uni)} fast={len(fast)}"
    for i, ((ra, re_), (ua, ue), (fa, fe)) in enumerate(zip(rtl, uni, fast)):
        if ra != re_:
            ok = False; lines.append(f"  [{i}] RTL act!=exp {ra:#010x}!={re_:#010x}")
        if ua != ue:
            ok = False; lines.append(f"  [{i}] unicorn act!=exp {ua:#010x}!={ue:#010x}")
        if fa != fe:
            ok = False; lines.append(f"  [{i}] fast act!=exp {fa:#010x}!={fe:#010x}")
        if not (ra == ua == fa):
            ok = False; lines.append(f"  [{i}] cross-act mismatch rtl={ra:#010x} uni={ua:#010x} fast={fa:#010x}")
        elif ra == re_:
            lines.append(f"  [{i}] OK act={ra:#010x} exp={re_:#010x}")
    return ok, "\n".join(lines)


def selftest_compare_unit() -> bool:
    """Negative: injected mismatch must be detected."""
    rtl = [(1, 1), (2, 2)]
    uni = [(1, 1), (2, 2)]
    fast_bad = [(1, 1), (9, 2)]  # act mismatch + cross mismatch
    ok, detail = compare_signatures("inj", rtl, uni, fast_bad)
    if ok:
        print("  selftest_compare_detects_mismatch: FAIL (did not catch)")
        return False
    print("  selftest_compare_detects_mismatch: PASS")
    return True



def selftest_discovery_order() -> bool:
    """HOME/shared and auth extracts are ordered before older cs150-1245 snapshots."""
    cands = discovery_candidates(ROOT)
    texts = [str(p) for p in cands]
    def idx(substr: str):
        for i, s in enumerate(texts):
            if substr in s:
                return i
        return None
    home_i = idx("/shared/cs150")
    auth_i = idx("cs150-auth-0ff61b6")
    old_i = idx("cs150-1245")
    ok = True
    if home_i is not None and old_i is not None and home_i > old_i:
        ok = False
    if auth_i is not None and old_i is not None and auth_i > old_i:
        ok = False
    if ok:
        print("  selftest_discovery_order: PASS")
    else:
        print(f"  selftest_discovery_order: FAIL order={texts[:8]}")
    return ok


def selftest_dump_be32_read_fail() -> bool:
    """Invalid guest address must report ok=0; harness must not treat that as data."""
    if not VV.is_file():
        print("  selftest_dump_be32_read_fail: FAIL missing vvizard")
        return False
    with tempfile.TemporaryDirectory(prefix="vv_dumpfail_") as td:
        bil = Path(td) / "b.bin"
        bil.write_bytes((0x0000000D).to_bytes(4, "big"))
        args = [
            str(VV), "--sync", "--no-window", "--frame-clock=emulated", "--quiet-mmio",
            "--no-random-ddr", "--no-bios", "--no-halt-on-quirk", "--max-insns", "20",
            "--cpu=unicorn", "--load", f"{bil}@0x60000000", "--entry", "0x60000000",
            "--dump-be32=0x90000000:1",
        ]
        r = subprocess.run(args, capture_output=True, env=ENV, timeout=15)
        err = r.stderr.decode(errors="replace")
        lines = [l for l in err.splitlines() if l.startswith("[dump-be32]")]
        if not lines or "ok=0" not in lines[0]:
            print(f"  selftest_dump_be32_read_fail: FAIL lines={lines!r}")
            return False
        fake = "[dump-be32] a=0x50000000 v=0x00000000 ok=0"
        m = DUMP.match(fake.strip())
        if not m or int(m.group(3)) != 0:
            print("  selftest_dump_be32_read_fail: FAIL regex")
            return False
        print("  selftest_dump_be32_read_fail: PASS")
        return True


def selftest_timeout(cs150: Path, work: Path) -> bool:
    """Negative: program that never finishes -> TIMEOUT visible fail."""
    # Assemble a tight infinite loop without DONE_MAGIC
    s = work / "hang.s"
    s.write_text("""
.set noreorder
.text
.global _start
_start:
1: j 1b
   nop
""")
    try:
        bil, hx = assemble(s, work)
    except SystemExit:
        # assemble helper dies process - use inline
        return False
    pairs, out, rc = run_rtl(cs150, hx, work, timeout_s=5, max_cycles=200)
    if "TIMEOUT:" in out or rc == 124:
        print("  selftest_rtl_timeout: PASS")
        return True
    print(f"  selftest_rtl_timeout: FAIL rc={rc}\n{out[-500:]}")
    return False


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cs150-root", default=None)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--selftest-only", action="store_true")
    ap.add_argument("--cases", default=None, help="comma-separated case stems; default all")
    args = ap.parse_args(argv)

    if not VV.is_file():
        die(f"missing vvizard binary at {VV}; run make first")

    vers = tool_versions()
    print("tools:", vers)
    if vers.get("iverilog") == "MISSING" or vers.get("vvp") == "MISSING":
        die("iverilog/vvp required for test-cpu-rtl (not a silent skip)", 2)
    if vers.get("mips-as") == "MISSING":
        die("mips-linux-gnu-as required to build case programs", 2)

    forced = bool(args.cs150_root or os.environ.get("CS150_ROOT"))
    cs150 = resolve_cs150(args.cs150_root)
    rev = rtl_revision(cs150)
    print(f"CS150_ROOT={cs150} ({'explicit' if forced else 'discovered'})")
    print(f"RTL_REVISION={rev}")

    fails = 0
    if not selftest_compare_unit():
        fails += 1
    if not selftest_discovery_order():
        fails += 1
    if not selftest_dump_be32_read_fail():
        fails += 1

    with tempfile.TemporaryDirectory(prefix="vv_rtl_") as td:
        work = Path(td)
        if not selftest_timeout(cs150, work):
            fails += 1
        if args.selftest_only:
            print(f"{'PASS' if not fails else 'FAIL'}: selftests")
            return 1 if fails else 0

        stems = sorted(p.stem for p in CASES.glob("*.s"))
        if args.cases:
            want = [x.strip() for x in args.cases.split(",") if x.strip()]
            stems = [s for s in stems if s in want]
        if not stems:
            die("no cases to run", 2)

        print(f"cases: {len(stems)} -> {', '.join(stems)}")
        for stem in stems:
            case_s = CASES / f"{stem}.s"
            t0 = time.time()
            try:
                bil, hx = assemble(case_s, work)
            except SystemExit as e:
                print(f"  {stem}: FAIL assemble")
                fails += 1
                continue
            rtl_pairs, rtl_out, rtl_rc = run_rtl(cs150, hx, work, args.timeout)
            if not rtl_pairs:
                print(f"  {stem}: FAIL rtl (no signature)\n{rtl_out[-800:]}")
                fails += 1
                continue
            # Prefer RTL-declared expected count for emu dump sizing
            uni_pairs, uni_out, uni_rc = run_emu("unicorn", bil, len(rtl_pairs), args.timeout)
            fast_pairs, fast_out, fast_rc = run_emu("fast", bil, len(rtl_pairs), args.timeout)
            ok, detail = compare_signatures(stem, rtl_pairs, uni_pairs, fast_pairs)
            dt = time.time() - t0
            status = "PASS" if ok else "FAIL"
            print(f"  {stem}: {status}  checks={len(rtl_pairs)}  ({dt:.2f}s)  [rtl/uni/fast compared]")
            if not ok:
                print(detail)
                if "DONE_MAGIC" in uni_out:
                    print("unicorn note:", [l for l in uni_out.splitlines() if "DONE" in l or "dump-be32" in l][:3])
                fails += 1
            else:
                # show one-line summary of acts
                acts = ",".join(f"{a:#x}" for a, _ in rtl_pairs[:8])
                print(f"    acts[:8]={acts}{'...' if len(rtl_pairs)>8 else ''}")

    total = len(stems) if not args.selftest_only else 0
    print(f"{'PASS' if not fails else 'FAIL'}: rtl-diff fails={fails} cases_run={total if not args.selftest_only else 'selftest'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
