#!/usr/bin/env python3
"""Isolated tests: optional-dep SKIP vs real FAIL for test-cpu-rtl.

Does not modify installed tools. Injects which()/paths via the harness API
and subprocess env (CS150_ROOT / PATH).
"""
from __future__ import annotations

import os, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE / "rtl"))
import run_rtl_diff as rtl  # noqa: E402

ENV_BASE = dict(os.environ)
ENV_BASE.pop("VVIZARD_QUIRKS", None)
ENV_BASE.pop("CS150_ROOT", None)


def _run_main(argv, env=None, which_override=None):
    """Call main with optional which injection via rtl._which hook if present."""
    old = getattr(rtl, "_which", None)
    if which_override is not None:
        rtl._which = which_override
    try:
        # Capture prints
        import io
        from contextlib import redirect_stdout, redirect_stderr
        out, err = io.StringIO(), io.StringIO()
        code = 0
        with redirect_stdout(out), redirect_stderr(err):
            try:
                code = rtl.main(argv)
            except SystemExit as e:
                code = int(e.code) if e.code is not None else 0
        return code, out.getvalue() + err.getvalue()
    finally:
        if which_override is not None:
            if old is None and hasattr(rtl, "_which"):
                # leave default if module sets it
                pass
            rtl._which = old if old is not None else getattr(rtl, "_which", None)


def test_missing_tools_reason_lists_each():
    def fake_which(cmd):
        return None  # all missing

    missing = rtl.missing_optional_tools(which=fake_which)
    for name in ("iverilog", "vvp", "mips-linux-gnu-as", "mips-linux-gnu-objcopy"):
        assert name in missing, f"expected {name} in {missing}"


def test_partial_tool_missing_only_that_one():
    present = {"vvp", "mips-linux-gnu-as", "mips-linux-gnu-objcopy"}

    def fake_which(cmd):
        return "/bin/true" if cmd in present else None

    missing = rtl.missing_optional_tools(which=fake_which)
    assert missing == ["iverilog"], missing


def test_skip_reason_explicit_bad_cs150_root(tmp_path: Path | None = None):
    td = Path(tempfile.mkdtemp(prefix="cs150_bad_"))
    reason = rtl.optional_dep_skip_reason(explicit=str(td), which=lambda c: "/x/" + c)
    assert reason is not None
    assert "CS150_ROOT" in reason or "CoreHarness" in reason or "cs150" in reason.lower()
    assert "SKIP" not in reason  # reason body only; printer adds SKIP


def test_skip_reason_no_discovery(monkeypatch_home=True):
    def fake_which(cmd):
        return "/usr/bin/" + cmd

    # Force empty discovery by using a root with no candidates that look like cs150
    # optional_dep_skip_reason(explicit=None) with discovery_candidates returning empty usable
    reason = rtl.optional_dep_skip_reason(
        explicit=None,
        which=fake_which,
        candidates=[Path("/nonexistent/a"), Path("/nonexistent/b")],
    )
    assert reason is not None
    assert "CS150_ROOT" in reason or "cs150" in reason.lower()


def test_no_skip_when_deps_present_auth():
    auth = Path("/workspace/mips-emu-plan/cs150-auth-0ff61b6")
    if not rtl._looks_like_cs150(auth):
        print("  skip unit: auth extract absent on this host")
        return True
    reason = rtl.optional_dep_skip_reason(explicit=str(auth), which=lambda c: "/usr/bin/" + c)
    assert reason is None, reason
    return True


def test_main_skips_exit0_missing_tool():
    """main() must exit 0 with SKIP line when iverilog missing (injected)."""
    auth = Path("/workspace/mips-emu-plan/cs150-auth-0ff61b6")
    if not auth.is_dir():
        print("  test_main_skips: no auth tree; use tmp incomplete")
        auth = Path(tempfile.mkdtemp())

    def fake_which(cmd):
        if cmd == "iverilog":
            return None
        return "/usr/bin/" + cmd

    # Ensure vvizard exists (else hard fail for build)
    if not rtl.VV.is_file():
        print("FAIL: vvizard missing — build responsibility, not SKIP")
        return False

    old = rtl._which
    rtl._which = fake_which
    try:
        import io
        from contextlib import redirect_stdout, redirect_stderr
        buf = io.StringIO()
        with redirect_stdout(buf), redirect_stderr(buf):
            # Clear CS150 from env; pass explicit so discovery not needed
            os.environ.pop("CS150_ROOT", None)
            code = rtl.main(["--cs150-root", str(auth), "--selftest-only"])
    except SystemExit as e:
        code = int(e.code or 0)
    finally:
        rtl._which = old
    text = buf.getvalue()
    assert code == 0, (code, text)
    assert "SKIP:" in text, text
    assert "iverilog" in text, text
    assert "PASS:" not in text or "cases_run=0" in text or "skipped" in text.lower()
    # Must not claim cases passed
    assert "cases_run=8" not in text
    return True


def test_main_skips_exit0_bad_cs150_root():
    if not rtl.VV.is_file():
        return False
    td = tempfile.mkdtemp(prefix="not_cs150_")
    import io
    from contextlib import redirect_stdout, redirect_stderr
    buf = io.StringIO()
    # All tools present (real which)
    with redirect_stdout(buf), redirect_stderr(buf):
        os.environ.pop("CS150_ROOT", None)
        try:
            code = rtl.main(["--cs150-root", td, "--selftest-only"])
        except SystemExit as e:
            code = int(e.code or 0)
    text = buf.getvalue()
    assert code == 0, (code, text[-500:])
    assert "SKIP:" in text, text
    assert "CS150_ROOT" in text or "CoreHarness" in text
    return True


def test_real_fail_signature_mismatch_not_skip():
    """Injected compare failure remains FAIL (exit != 0), not SKIP."""
    ok, _ = rtl.compare_signatures("x", [(1, 1)], [(1, 1)], [(9, 1)])
    assert ok is False
    # selftest_compare_unit currently expects to detect and return True (test passed)
    assert rtl.selftest_compare_unit() is True
    return True


def test_vvizard_missing_is_hard_fail_not_skip(tmp_path_name="x"):
    old_vv = rtl.VV
    rtl.VV = Path("/nonexistent/vvizard-binary-absent")
    import io
    from contextlib import redirect_stdout, redirect_stderr
    buf = io.StringIO()
    code = 0
    with redirect_stdout(buf), redirect_stderr(buf):
        try:
            code = rtl.main(["--selftest-only"])
        except SystemExit as e:
            code = int(e.code) if e.code is not None else 0
    rtl.VV = old_vv
    text = buf.getvalue()
    assert code != 0, text
    assert "SKIP:" not in text
    assert "vvizard" in text.lower() or "missing" in text.lower()
    return True


def test_incomplete_rtl_missing_cpumips_skips():
    td = Path(tempfile.mkdtemp(prefix="cs150_partial_"))
    (td / "hardware/sim-icarus/tb").mkdir(parents=True)
    (td / "hardware/sim-icarus/tb/CoreHarness.v").write_text("// stub\n")
    (td / "hardware/src/innerds").mkdir(parents=True)
    reason = rtl.optional_dep_skip_reason(explicit=str(td), which=lambda c: "/usr/bin/" + c)
    assert reason is not None and "CPUMIPS.v" in reason, reason


def test_present_broken_rtl_compiles_as_fail_not_skip():
    """Tree looks complete but Verilog is junk → iverilog failure is FAIL, not SKIP."""
    if not rtl.VV.is_file():
        raise AssertionError("vvizard required for this negative test")
    if rtl.missing_optional_tools():
        print("  (tools missing on host — skip this negative)")
        return True
    td = Path(tempfile.mkdtemp(prefix="cs150_junk_"))
    (td / "hardware/sim-icarus/tb").mkdir(parents=True)
    (td / "hardware/src/cpu").mkdir(parents=True)
    (td / "hardware/src/innerds").mkdir(parents=True)
    (td / "hardware/sim-icarus/tb/CoreHarness.v").write_text("module CoreHarness; endmodule\n")
    (td / "hardware/src/cpu/CPUMIPS.v").write_text("module CPUMIPS; this is not valid verilog!!!\n")
    reason = rtl.optional_dep_skip_reason(explicit=str(td), which=rtl._which)
    assert reason is None, f"should not skip complete-looking tree: {reason}"
    import io
    from contextlib import redirect_stdout, redirect_stderr
    buf = io.StringIO()
    with redirect_stdout(buf), redirect_stderr(buf):
        try:
            code = rtl.main(["--cs150-root", str(td), "--cases", "alu_wrap"])
        except SystemExit as e:
            code = int(e.code or 0)
    text = buf.getvalue()
    assert code != 0, text[-800:]
    assert "SKIP:" not in text
    assert "FAIL" in text or "iverilog failed" in text or "ERROR:" in text
    return True



def main():
    fails = 0
    tests = [
        test_missing_tools_reason_lists_each,
        test_partial_tool_missing_only_that_one,
        test_skip_reason_explicit_bad_cs150_root,
        test_skip_reason_no_discovery,
        test_no_skip_when_deps_present_auth,
        test_main_skips_exit0_missing_tool,
        test_main_skips_exit0_bad_cs150_root,
        test_real_fail_signature_mismatch_not_skip,
        test_vvizard_missing_is_hard_fail_not_skip,
        test_incomplete_rtl_missing_cpumips_skips,
        test_present_broken_rtl_compiles_as_fail_not_skip,
    ]
    for t in tests:
        name = t.__name__
        try:
            t()
            print(f"  {name}: PASS")
        except AssertionError as e:
            print(f"  {name}: FAIL {e}")
            fails += 1
        except AttributeError as e:
            print(f"  {name}: FAIL (missing API — expected RED) {e}")
            fails += 1
        except Exception as e:
            print(f"  {name}: FAIL {type(e).__name__}: {e}")
            fails += 1
    print(f"{'PASS' if not fails else 'FAIL'}: rtl-skip {len(tests) - fails}/{len(tests)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
