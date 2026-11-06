# Tri-way CPU RTL differential harness

Compares the same MIPS machine-code cases on:

1. `--cpu=unicorn` (reference emulator)
2. `--cpu=fast` (BB interpreter)
3. User CPUMIPS Verilog via Icarus `CoreHarness` (read-only cs150 tree)

## Requirements (optional for `make test-cpu-rtl`)

- `iverilog` / `vvp`
- `mips-linux-gnu-as` / `objcopy`
- `CS150_ROOT` (or `--cs150-root`) pointing at a cs150 checkout that contains:
  - `hardware/sim-icarus/tb/CoreHarness.v`
  - `hardware/src/cpu/CPUMIPS.v`
  - `hardware/src/innerds/` (RegFile, ALU, InstructionControl, ByteAccess4, …)

No cs150/Verilog files are modified. The TB `CPU_sig_tb.v` lives in this directory.

## Command

```bash
make test-cpu-rtl CS150_ROOT=/path/to/cs150
# or
CS150_ROOT=/path/to/cs150 python3 tests/test_cpu_rtl.py
```

### Skip vs fail

| Situation | Result |
|-----------|--------|
| Missing `iverilog` / `vvp` / `mips-linux-gnu-as` / `objcopy` | **SKIP** exit 0 — prints `SKIP: missing tool(s): … (0 cases run)` |
| Missing / incomplete `CS150_ROOT` (no silent discovery fallback when explicit) | **SKIP** exit 0 — prints `SKIP: CS150_ROOT=… incomplete/missing RTL (…) (0 cases run)` or not-found |
| Missing `vvizard` binary | **FAIL** — run `make` first (not an optional dep) |
| Tool present but iverilog compile error, vvp crash, timeout, signature mismatch, dump parse/`ok=0` | **FAIL** (not SKIP) |

`make test` does not include `test-cpu-rtl`; this target stays optional. Skip is never reported as PASS.

## Signature

Programs use `tests/check.inc` (`CHECK` / `END_CHECKS` → DONE_MAGIC `0x600DC0DE` + pairs at `0x50000400`).
RTL prints `SIG` lines; emulators dump the same table via `--dump-be32`.

Compared: each backend’s actual==expected, and actuals equal across all three.
Not compared: wall time, cycle count, IRQ/device timing (out of scope).

## Quirks

Default vvizard quirks match current MIPS150 RTL behavior used by the corpus:
`lwl`→0 (`+lwlr`), addiu wrap (`+overflow`), etc. **`lb`/`lh` sign-extend**
(MIPS-I / current CPUMIPS; the `lb-sign` quirk defaults **off** = current HW).
Case `lb_sext` encodes that. Undefined / uninit / branch-in-delay are not oracles.

Prefer `CS150_ROOT` / `--cs150-root` pointing at the user's current tree
(e.g. UBU24 `/home/ejr/shared/cs150` or the reviewer extract
`…/cs150-auth-0ff61b6`). Unset discovery prefers `$HOME/shared/cs150` and
auth extracts before older snapshots like `cs150-1245`; always check the printed
`RTL_REVISION`.

## Self-tests

- Injected signature mismatch must FAIL
- Infinite-loop program must TIMEOUT
- Discovery order prefers live/auth trees before older snapshots
- `--dump-be32` on an unmapped address reports `ok=0` (no fake pass)
- Optional-dep skip vs real-fail: `python3 tests/test_cpu_rtl_skip.py`
