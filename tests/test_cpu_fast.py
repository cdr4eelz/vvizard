#!/usr/bin/env python3
"""Fast CPU backend tests vs Unicorn reference (default quirks = MIPS150 L0).

Default CPU is fast. Unicorn is the explicit reference (--cpu=unicorn). These exercise fast and differential behavior:
hwprobe (45 checks), quirk on/off matrices, ALU/memory/branch edges, and
explicit unsupported diagnostics. MIPS-I "spec" oracles are not invented for
undefined cases (branch-in-delay, uninitialized regs).
"""
import os, re, subprocess, sys, tempfile
from vvprog import prog

HERE = os.path.dirname(os.path.abspath(__file__))
VV = os.path.join(os.path.dirname(HERE), "vvizard")
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser("~/.local/lib"))
ENV.pop("VVIZARD_QUIRKS", None)
BASE = ["--sync", "--no-window", "--frame-clock=emulated", "--quiet-mmio",
        "--no-random-ddr", "--no-bios", "--max-insns", "1000000", "--no-halt-on-quirk"]
BUGS = "dcache-stale,tx-busy,rx-overrun,irq-storm,undef-reg,gp-busy"


def binfile(name):
    return prog(name)[0]


def run_cpu(cpu, name, addr, *extra, at_max=None):
    args = [VV, *BASE, f"--cpu={cpu}", f"--quiet-quirk={BUGS}",
            "--load", f"{binfile(name)}@{addr:#x}", "--entry", f"{addr:#x}", *extra]
    if at_max is not None:
        args = [a if a != "1000000" else str(at_max) for a in args]
    return subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, env=ENV)


def expect_pass(r, label):
    err = r.stderr.decode(errors="replace")
    if r.returncode != 0:
        print(f"  {label}: FAIL rc={r.returncode}\n{err[-1500:]}")
        return False
    if "[expect]" in err and " FAIL" in err:
        print(f"  {label}: FAIL expect\n{err[-1500:]}")
        return False
    print(f"  {label}: PASS")
    return True


def regs_from_stderr(err):
    """Parse '  t0=XXXXXXXX' style dump lines into dict."""
    out = {}
    for m in re.finditer(r"\b([a-z0-9]+)=([0-9a-fA-F]{8})\b", err):
        out[m.group(1)] = int(m.group(2), 16)
    return out


def differential(name, addr, *extra, label=None, expect_same_rc=True):
    """Run unicorn and fast with identical args; compare exit + expect lines + key GPRs."""
    label = label or f"diff_{name}"
    ru = run_cpu("unicorn", name, addr, *extra)
    rf = run_cpu("fast", name, addr, *extra)
    eu = ru.stderr.decode(errors="replace")
    ef = rf.stderr.decode(errors="replace")
    if expect_same_rc and ru.returncode != rf.returncode:
        print(f"  {label}: FAIL rc unicorn={ru.returncode} fast={rf.returncode}\nfast:\n{ef[-800:]}")
        return False
    # Compare [expect] verdicts
    def expects(err):
        return [l for l in err.splitlines() if l.startswith("[expect]")]
    if expects(eu) != expects(ef):
        print(f"  {label}: FAIL expect mismatch\n  U:{expects(eu)}\n  F:{expects(ef)}")
        return False
    # Compare a few GPRs commonly dumped
    gu, gf = regs_from_stderr(eu), regs_from_stderr(ef)
    for k in ("t0", "t1", "t2", "t3", "t4", "s3", "s4", "s5", "ra", "v0", "a0", "a1"):
        if k in gu and k in gf and gu[k] != gf[k]:
            print(f"  {label}: FAIL ${k} unicorn={gu[k]:#x} fast={gf[k]:#x}")
            return False
    print(f"  {label}: PASS")
    return True


def t_help_mentions_cpu():
    r = subprocess.run([VV, "--help"], capture_output=True, env=ENV)
    text = (r.stdout + r.stderr).decode(errors="replace")
    ok = "--cpu=" in text and "default fast" in text and "unicorn" in text
    print(f"  help_cpu: {'PASS' if ok else 'FAIL'}")
    return ok


def t_default_is_fast_loop():
    r = subprocess.run(
        [VV, *BASE, "--load", f"{binfile('loop')}@0x40000000", "--entry", "0x40000000",
         "--expect", "t0=30", "--expect", "t2=0x11e"],
        stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
    err = r.stderr.decode(errors="replace")
    if "cpu=fast" not in err:
        print(f"  default_fast_loop: FAIL run line not fast\n{err[-400:]}")
        return False
    return expect_pass(r, "default_fast_loop")


def t_default_matches_explicit_fast():
    bil = binfile("loop")
    args = [VV, *BASE, "--load", f"{bil}@0x40000000", "--entry", "0x40000000",
            "--expect", "t0=30", "--expect", "t2=0x11e"]
    rd = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
    rf = subprocess.run(args[:1] + BASE + ["--cpu=fast"] + args[1+len(BASE):],
                        stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
    # rebuild cleanly
    common = ["--load", f"{bil}@0x40000000", "--entry", "0x40000000",
              "--expect", "t0=30", "--expect", "t2=0x11e"]
    rd = subprocess.run([VV, *BASE, *common], stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
    rf = subprocess.run([VV, *BASE, "--cpu=fast", *common], stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
    ok = rd.returncode == 0 and rf.returncode == 0 and b"cpu=fast" in rd.stderr and b"cpu=fast" in rf.stderr
    print(f"  default_matches_explicit_fast: {'PASS' if ok else 'FAIL'}")
    return ok


def t_unicorn_loop_explicit():
    r = run_cpu("unicorn", "loop", 0x40000000, "--expect", "t0=30", "--expect", "t2=0x11e")
    return expect_pass(r, "unicorn_loop")


def t_fast_loop():
    r = run_cpu("fast", "loop", 0x40000000, "--expect", "t0=30", "--expect", "t2=0x11e")
    return expect_pass(r, "fast_loop")


def t_fast_alias():
    r = run_cpu("fast", "alias", 0xC0000180, "--expect", "t3=0xbeef", "--expect", "t4=0xbeef")
    return expect_pass(r, "fast_alias")


def t_fast_hwprobe():
    """All 45 MIPS150 checks must pass under default quirks (s5=0, s3=45)."""
    r = run_cpu("fast", "hwprobe", 0x60000000, "--expect", "s5=0", "--expect", "s3=45",
                "--expect", "s4=0")
    return expect_pass(r, "fast_hwprobe")


def t_unicorn_hwprobe():
    r = run_cpu("unicorn", "hwprobe", 0x60000000, "--expect", "s5=0", "--expect", "s3=45",
                "--expect", "s4=0")
    return expect_pass(r, "unicorn_hwprobe")


def t_diff_hwprobe():
    return differential("hwprobe", 0x60000000, "--expect", "s5=0", "--expect", "s3=45",
                        "--expect", "s4=0", label="diff_hwprobe")


def t_fast_emu_lwlr_on():
    r = run_cpu("fast", "quirk/emu_lwlr", 0x40000000, "--quirks=+lwlr",
                "--expect", "t2=0", "--expect", "t0=0x50000000")
    return expect_pass(r, "fast_emu_lwlr_on")


def t_fast_emu_lwlr_off_falls_back_or_matches():
    """With lwlr off, Unicorn merges; fast may reject unsupported merge rather than fake it."""
    ru = run_cpu("unicorn", "quirk/emu_lwlr", 0x40000000, "--quirks=-lwlr",
                 "--expect", "t2=0x22334477")
    rf = run_cpu("fast", "quirk/emu_lwlr", 0x40000000, "--quirks=-lwlr",
                 "--expect", "t2=0x22334477")
    if rf.returncode == 0 and b"PASS" in rf.stderr and b" FAIL" not in rf.stderr:
        print("  fast_emu_lwlr_off: PASS (matches Unicorn merge)")
        return True
    err = rf.stderr.decode(errors="replace")
    if "[fast] unsupported" in err or "not implemented" in err.lower():
        print("  fast_emu_lwlr_off: PASS (explicit unsupported; unicorn still reference)")
        return ru.returncode == 0
    print(f"  fast_emu_lwlr_off: FAIL\n{err[-1200:]}")
    return False


def t_fast_emu_hilo_on():
    r = run_cpu("fast", "quirk/emu_hilo", 0x40000000, "--quirks=+hilo", "--expect", "t2=0")
    return expect_pass(r, "fast_emu_hilo_on")


def t_fast_emu_overflow_on():
    r = run_cpu("fast", "quirk/emu_overflow", 0x40000000, "--quirks=+overflow",
                "--expect", "t2=0x80000000")
    return expect_pass(r, "fast_emu_overflow_on")


def t_fast_emu_load_use_on():
    r = run_cpu("fast", "quirk/emu_load_use", 0x40000000, "--quirks=+load-use",
                "--expect", "t3=1", "--expect", "t2=0x99")
    return expect_pass(r, "fast_emu_load_use_on")



def t_fast_lwlr_mmio_side_read():
    """lwl of UARX_DATA under +lwlr must pop RX (vv_mmio_side_read), matching quirk.c.
    --uart-in A; after VALID wait, lwl DATA zeros rt but clears VALID. Differential vs unicorn.
    """
    extra = ["--quirks=+lwlr", "--uart-in", "A", "--uart-rx-gap", "0",
             "--expect", "t2=0", "--expect", "t3=0"]
    ru = run_cpu("unicorn", "quirk/emu_lwlr_mmio", 0x40000000, *extra, at_max=200000)
    rf = run_cpu("fast", "quirk/emu_lwlr_mmio", 0x40000000, *extra, at_max=200000)
    if not expect_pass(ru, "unicorn_lwlr_mmio_side"):
        return False
    if not expect_pass(rf, "fast_lwlr_mmio_side"):
        return False
    return differential("quirk/emu_lwlr_mmio", 0x40000000, *extra, label="diff_lwlr_mmio_side")


def t_fast_threads_windowed():
    """--cpu=fast --threads=on --window uses emu thread + display (Xvfb if available)."""
    import shutil
    args = [VV, "--frame-clock=emulated", "--quiet-mmio", "--no-random-ddr", "--no-bios",
            "--no-halt-on-quirk", "--cpu=fast", "--threads=on", "--window",
            "--load", f"{binfile('loop')}@0x40000000", "--entry", "0x40000000",
            "--max-insns", "5000"]
    if shutil.which("xvfb-run"):
        args = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + args
    else:
        print("  fast_threads_windowed: SKIP (no xvfb-run)")
        return True  # optional dep — count as non-fail; suite notes SKIP in message
    r = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, env=ENV, timeout=30)
    err = r.stderr.decode(errors="replace")
    ok = (r.returncode == 0 and "cpu=fast" in err and "emulator + display" in err
          and "forcing --sync" not in err and "does not support threaded" not in err)
    print(f"  fast_threads_windowed: {'PASS' if ok else 'FAIL'}\n{err[-500:] if not ok else ''}")
    return ok



def t_duty_cycle_reported():
    """[duty] end-of-run line: CPU vs (GPU model + display); GPU time on gpcmds sync."""
    # CPU-only: loop should report 100% CPU, 0% GPU/display (headless --sync)
    r = run_cpu("fast", "loop", 0x40000000, "--expect", "t0=30", at_max=50000)
    err = r.stderr.decode(errors="replace")
    if r.returncode != 0 or "[duty] host MIPS CPU" not in err or "CPU vs render" not in err:
        print(f"  duty_loop: FAIL\n{err[-800:]}"); return False
    if "GPU model 0.000 ms" not in err or "display present 0.000 ms" not in err:
        # allow tiny noise but expect zeros headless no-gpu
        if "GPU model 0." not in err:
            print(f"  duty_loop: FAIL expected zero GPU\n{err[-400:]}"); return False
    print("  duty_loop: PASS")
    # GPU draw: gpcmds+gplaunch — GPU model bucket > 0 for fast and unicorn
    ok = True
    for cpu in ("fast", "unicorn"):
        args = [VV, *BASE, f"--cpu={cpu}",
                "--load", f"{binfile('gpcmds')}@0x12003000",
                "--load", f"{binfile('gplaunch')}@0x40000000",
                "--entry", "0x40000000"]
        rr = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
        e = rr.stderr.decode(errors="replace")
        m = re.search(r"\[duty\] host MIPS CPU ([0-9.]+) ms \(([0-9.]+)%\), GPU model ([0-9.]+) ms \(([0-9.]+)%\), display present ([0-9.]+) ms", e)
        if rr.returncode != 0 or not m:
            print(f"  duty_gpcmds_{cpu}: FAIL\n{e[-800:]}"); ok = False; continue
        gpu_ms = float(m.group(3)); cpu_pct = float(m.group(2)); rend_need = "CPU vs render" in e
        if gpu_ms <= 0.0 or not rend_need:
            print(f"  duty_gpcmds_{cpu}: FAIL gpu_ms={gpu_ms} line={m.group(0)}"); ok = False; continue
        print(f"  duty_gpcmds_{cpu}: PASS (CPU {m.group(1)}ms {cpu_pct}%, GPU {gpu_ms}ms)")
    return ok



def t_sync_overlap_delay():
    """[sync] approx overlap% vs blocked/delayed%; --sync overlap ~0; threaded windowed sane."""
    import shutil
    # --sync headless gpcmds: expect [sync] line, non-neg, overlap near 0
    ok = True
    for cpu in ("fast", "unicorn"):
        args = [VV, *BASE, f"--cpu={cpu}",
                "--load", f"{binfile('gpcmds')}@0x12003000",
                "--load", f"{binfile('gplaunch')}@0x40000000",
                "--entry", "0x40000000"]
        r = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
        e = r.stderr.decode(errors="replace")
        m = re.search(
            r"\[sync\] approx overlap ([0-9.]+) ms \(([0-9.]+)% of wall\), "
            r"blocked/delayed ([0-9.]+) ms \(([0-9.]+)% of wall\)", e)
        if r.returncode != 0 or not m:
            print(f"  sync_sync_{cpu}: FAIL\n{e[-800:]}"); ok = False; continue
        ovr, ovr_pct, blk, blk_pct = map(float, m.groups())
        if ovr < 0 or blk < 0 or ovr_pct < 0 or blk_pct < 0:
            print(f"  sync_sync_{cpu}: FAIL negative {m.group(0)}"); ok = False; continue
        # --sync: no parallel threads for display; overlap proxy should be ~0
        if ovr_pct > 5.0 or ovr > 1.0:
            print(f"  sync_sync_{cpu}: FAIL expected ~0 overlap got {m.group(0)}"); ok = False; continue
        if "--sync (expect ~0 overlap)" not in e:
            print(f"  sync_sync_{cpu}: FAIL missing --sync note"); ok = False; continue
        print(f"  sync_sync_{cpu}: PASS ({m.group(0)})")

    if not shutil.which("xvfb-run"):
        print("  sync_threads_windowed: SKIP (no xvfb-run)")
        return ok

    for cpu in ("fast", "unicorn"):
        args = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24",
                VV, "--window", "--threads=on", "--frame-clock=emulated", "--quiet-mmio",
                "--no-random-ddr", "--no-bios", "--no-halt-on-quirk", f"--cpu={cpu}",
                "--max-insns", "500000",
                "--load", f"{binfile('gpcmds')}@0x12003000",
                "--load", f"{binfile('gplaunch')}@0x40000000",
                "--entry", "0x40000000"]
        r = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, env=ENV, timeout=60)
        e = r.stderr.decode(errors="replace")
        m = re.search(
            r"\[sync\] approx overlap ([0-9.]+) ms \(([0-9.]+)% of wall\), "
            r"blocked/delayed ([0-9.]+) ms \(([0-9.]+)% of wall\)", e)
        if r.returncode != 0 or not m:
            print(f"  sync_threads_{cpu}: FAIL\n{e[-800:]}"); ok = False; continue
        ovr, ovr_pct, blk, blk_pct = map(float, m.groups())
        if ovr < 0 or blk < 0:
            print(f"  sync_threads_{cpu}: FAIL negative"); ok = False; continue
        if "emulator + display" not in e:
            print(f"  sync_threads_{cpu}: FAIL not threaded display\n{e[-400:]}"); ok = False; continue
        print(f"  sync_threads_{cpu}: PASS ({m.group(0)})")
    return ok


# ---- Quirk on matrices (MIPS150 intentional deviations; Unicorn reference under +quirk) ----
EMU_ON = [
    ("quirk/emu_hilo", 0x40000000, ["--quirks=+hilo"], ["t2=0"]),
    ("quirk/emu_overflow", 0x40000000, ["--quirks=+overflow"], ["t2=0x80000000"]),
    ("quirk/emu_load_use", 0x40000000, ["--quirks=+load-use"], ["t3=1", "t2=0x99"]),
    ("quirk/emu_lwlr", 0x40000000, ["--quirks=+lwlr"], ["t2=0", "t0=0x50000000"]),
    ("quirk/emu_lb_sign", 0x40000000, ["--quirks=+lb-sign"], ["t2=0x80", "t3=0x8001"]),
    ("quirk/emu_jalr_rd", 0x40000000, ["--quirks=+jalr-rd"], ["ra=0x40000014", "t1=0"]),
    ("quirk/emu_bal_untaken", 0x40000000, ["--quirks=+bal-untaken"], ["ra=0"]),
    ("quirk/emu_cop0", 0x40000000, ["--quirks=+cop0"], ["t2=0", "t3=0xffff"]),
    ("quirk/emu_syscall", 0x40000000, ["--quirks=+syscall"], ["t2=2"]),
    ("quirk/emu_movz", 0x40000000, ["--quirks=+reserved"], ["t2=0"]),
    ("quirk/emu_tgei", 0x40000000, ["--quirks=+reserved"], ["t2=5"]),
    ("quirk/emu_unaligned", 0x40000000, ["--quirks=+unaligned"], ["t2=0", "t4=0x11223344"]),
    ("quirk/emu_unm_load", 0x40000000, ["--quirks=+unmapped"], ["t1=0", "t2=6"]),
]


def t_emu_on_matrix():
    ok = True
    for name, addr, flags, expects in EMU_ON:
        extra = list(flags)
        for e in expects:
            extra += ["--expect", e]
        ru = run_cpu("unicorn", name, addr, *extra)
        rf = run_cpu("fast", name, addr, *extra)
        label = f"matrix_on/{os.path.basename(name)}"
        if not expect_pass(rf, f"fast_{label}"):
            ok = False
            continue
        # Differential vs unicorn when unicorn also passes
        if ru.returncode == 0:
            eu, ef = ru.stderr.decode(), rf.stderr.decode()
            if [l for l in eu.splitlines() if l.startswith("[expect]")] != \
               [l for l in ef.splitlines() if l.startswith("[expect]")]:
                print(f"  diff_{label}: FAIL expect mismatch")
                ok = False
            else:
                print(f"  diff_{label}: PASS")
        else:
            print(f"  diff_{label}: SKIP (unicorn rc={ru.returncode})")
    return ok


# Off-quirk cases where fast must match Unicorn OR emit explicit unsupported
EMU_OFF = [
    # must_match=False => allow explicit [fast] unsupported (no silent wrong result)
    ("quirk/emu_hilo", 0x40000000, ["--quirks=-hilo"], ["t2=15"], False),  # real HI/LO needs Unicorn
    ("quirk/emu_load_use", 0x40000000, ["--quirks=-load-use"], ["t3=0x99", "t2=0x99"], True),
    ("quirk/emu_lb_sign", 0x40000000, ["--quirks=-lb-sign"], ["t2=0xffffff80", "t3=0xffff8001"], True),
    ("quirk/emu_jalr_rd", 0x40000000, ["--quirks=-jalr-rd"], ["t1=0x40000014", "ra=0"], True),
    ("quirk/emu_bal_untaken", 0x40000000, ["--quirks=-bal-untaken"], ["ra=0x40000010"], True),
    ("quirk/emu_cop0", 0x40000000, ["--quirks=-cop0"], ["t2=0x1234"], True),
    ("quirk/emu_movz", 0x40000000, ["--quirks=-reserved"], ["t2=7"], False),  # real movz needs Unicorn
    ("quirk/emu_lwlr", 0x40000000, ["--quirks=-lwlr"], ["t2=0x22334477"], False),
]


def t_emu_off_matrix():
    ok = True
    for name, addr, flags, expects, must_match in EMU_OFF:
        extra = list(flags)
        for e in expects:
            extra += ["--expect", e]
        ru = run_cpu("unicorn", name, addr, *extra)
        rf = run_cpu("fast", name, addr, *extra)
        label = f"matrix_off/{os.path.basename(name)}"
        ef = rf.stderr.decode(errors="replace")
        if rf.returncode == 0 and " FAIL" not in ef:
            print(f"  fast_{label}: PASS")
            continue
        if (not must_match) and ("[fast] unsupported" in ef or "not implemented" in ef.lower()):
            if ru.returncode != 0:
                print(f"  fast_{label}: FAIL (unsupported ok but unicorn reference failed rc={ru.returncode})")
                ok = False
            else:
                print(f"  fast_{label}: PASS (explicit unsupported; unicorn still reference)")
            continue
        print(f"  fast_{label}: FAIL\n{ef[-800:]}")
        ok = False
    return ok


def t_diff_loop_alias():
    a = differential("loop", 0x40000000, "--expect", "t0=30", "--expect", "t2=0x11e", label="diff_loop")
    b = differential("alias", 0xC0000180, "--expect", "t3=0xbeef", "--expect", "t4=0xbeef", label="diff_alias")
    return a and b


def t_diff_memory_sign():
    """lb/lh sign: default quirks include +lb-sign (MIPS150 zero-extend)."""
    return differential("quirk/emu_lb_sign", 0x40000000, "--quirks=+lb-sign",
                        "--expect", "t2=0x80", "--expect", "t3=0x8001",
                        label="diff_lb_sign_on")


def t_asm_alu_edges():
    """Inline tiny big-endian word programs: addi/addu wrap, shifts, slt — defined MIPS150 behavior."""
    # Helper: assemble via raw words written as a .bin through Python (no gas needed for these)
    def words_prog(words, expects, label):
        # Build a tiny ELF-less raw blob loaded at 0x40000000; vvizard loads raw bins
        blob = b"".join(w.to_bytes(4, "big") for w in words)
        fd, path = tempfile.mkstemp(suffix=".bin")
        os.write(fd, blob); os.close(fd)
        try:
            args_u = [VV, *BASE, "--cpu=unicorn", f"--quiet-quirk={BUGS}",
                      "--load", f"{path}@0x40000000", "--entry", "0x40000000", *expects]
            args_f = [VV, *BASE, "--cpu=fast", f"--quiet-quirk={BUGS}",
                      "--load", f"{path}@0x40000000", "--entry", "0x40000000", *expects]
            ru = subprocess.run(args_u, stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
            rf = subprocess.run(args_f, stdin=subprocess.DEVNULL, capture_output=True, env=ENV)
            if not expect_pass(rf, f"fast_{label}"):
                return False
            eu, ef = ru.stderr.decode(), rf.stderr.decode()
            if [l for l in eu.splitlines() if l.startswith("[expect]")] != \
               [l for l in ef.splitlines() if l.startswith("[expect]")]:
                print(f"  diff_{label}: FAIL")
                return False
            print(f"  diff_{label}: PASS")
            return True
        finally:
            os.unlink(path)

    ok = True
    # lui t0,0x7fff; ori t0,t0,0xffff; addiu t1,t0,1; break
    # -> t1 = 0x80000000 (wrap, no trap under +overflow) — MIPS150 overflow quirk
    ok &= words_prog([
        0x3c087fff, 0x3508ffff, 0x25090001, 0x0000000d,
    ], ["--quirks=+overflow", "--expect", "t1=0x80000000"], "alu_addiu_wrap")
    # lui t0,0x8000; sra t1,t0,31; srl t2,t0,31; break
    # sra rd,rt,sa = SPECIAL|rt<<16|rd<<11|sa<<6|3
    ok &= words_prog([
        0x3c088000, 0x00084fc3, 0x000857c2, 0x0000000d,
    ], ["--expect", "t1=0xffffffff", "--expect", "t2=1"], "alu_sra_srl")
    # ori t0,$0,5; sll t1,t0,2; break -> t1=20
    ok &= words_prog([
        0x34080005, 0x00084880, 0x0000000d,
    ], ["--expect", "t0=5", "--expect", "t1=20"], "alu_sll")
    # lui t0,0x8000; slt t1,t0,$0; sltu t2,t0,$0; break
    ok &= words_prog([
        0x3c088000, 0x0100482a, 0x0100502b, 0x0000000d,
    ], ["--expect", "t1=1", "--expect", "t2=0"], "alu_slt_sltu")
    # beq taken + delay: ori t0,1; beq t0,t0,+2; ori t1,0x11; ori t1,0x22; break
    # offset +2 => target = beq_pc+4+8 = break; delay sets t1=0x11; 0x22 skipped
    ok &= words_prog([
        0x34080001, 0x11080002, 0x34090011, 0x34090022, 0x0000000d,
    ], ["--expect", "t1=0x11"], "branch_delay_beq")
    # jal link: jal to break; nop delay — ra = jal_pc+8
    ok &= words_prog([
        0x0c100000 + 2,  # jal 0x40000008
        0x00000000,
        0x0000000d,
    ], ["--expect", "ra=0x40000008"], "jal_link")
    # load delay (load-use on): store then lw / use in next insn sees OLD value
    # Covered by emu_load_use differential; keep bounded here.
    return ok


def main():
    if not os.path.isfile(VV):
        print(f"FAIL: missing binary {VV}", file=sys.stderr)
        return 1
    tests = [
        t_help_mentions_cpu,
        t_default_is_fast_loop,
        t_default_matches_explicit_fast,
        t_unicorn_loop_explicit,
        t_unicorn_hwprobe,
        t_fast_loop,
        t_fast_alias,
        t_fast_hwprobe,
        t_diff_hwprobe,
        t_fast_emu_lwlr_on,
        t_fast_emu_lwlr_off_falls_back_or_matches,
        t_fast_emu_hilo_on,
        t_fast_emu_overflow_on,
        t_fast_emu_load_use_on,
        t_fast_lwlr_mmio_side_read,
        t_fast_threads_windowed,
        t_duty_cycle_reported,
        t_sync_overlap_delay,
        t_emu_on_matrix,
        t_emu_off_matrix,
        t_diff_loop_alias,
        t_diff_memory_sign,
        t_asm_alu_edges,
    ]
    fails = 0
    for t in tests:
        fails += not t()
    print(f"{'PASS' if not fails else 'FAIL'}: {len(tests) - fails}/{len(tests)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
