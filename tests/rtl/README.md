# Tri-way CPU RTL differential harness

Compares the same MIPS machine-code cases on:

1. `--cpu=unicorn` (reference emulator)
2. `--cpu=fast` (BB interpreter)
3. User CPUMIPS Verilog via Icarus `CoreHarness` (read-only cs150 tree)

## Requirements

- `iverilog` / `vvp`
- `mips-linux-gnu-as` / `objcopy`
- `CS150_ROOT` pointing at a cs150 checkout that contains:
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

Missing tools or `CS150_ROOT` → **hard FAIL** (not a silent skip).

## Signature

Programs use `tests/check.inc` (`CHECK` / `END_CHECKS` → DONE_MAGIC `0x600DC0DE` + pairs at `0x50000400`).
RTL prints `SIG` lines; emulators dump the same table via `--dump-be32`.

Compared: each backend’s actual==expected, and actuals equal across all three.
Not compared: wall time, cycle count, IRQ/device timing (out of scope).

## Quirks

Default vvizard quirks match MIPS150 RTL (lb/lh zero-extend, lwl→0, overflow wrap, …).
Undefined / uninit / branch-in-delay are not used as oracles.

## Self-tests

- Injected signature mismatch must FAIL
- Infinite-loop program must TIMEOUT
