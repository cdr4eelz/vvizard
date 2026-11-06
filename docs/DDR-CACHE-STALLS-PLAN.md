# DDR2 delay + icache/dcache stalls (research notes)

Branch: `e/ddr-cache-stalls` (from `e/personal-fast-mips` @ `26ecbdd`).
PR for the fast-cpu work: https://github.com/cdr4eelz/vvizard/pull/2
Author notes: Erik Rogers. **No emulator DDR/cache implementation in the first commit — research only.**

## Design gate

- **Caches and DDR miss stalls are active only when DDR delays are emulated.**
- Default `--cpu=fast` stays the current **no-DDR / 1 cycle per insn** performance path.
- Proposed opt-in (name TBD): e.g. `--mem=ddr` / `--ddr-stalls` that enables approximate I$/D$ + miss latency; without it, behavior matches today's fast path.

## What the docs / RTL say

Sources: cs150 ISE `0ff61b6` (`/home/ejr/shared/cs150` on UBU24; local auth extract `cs150-auth-0ff61b6`), vvizard `docs/MULTITHREADING-PLAN.md`, `QUIRKS.md`, `README.md`.

| Fact | Where |
|---|---|
| CPU / caches / RequestController @ **50 MHz**; MIG PHY @ **200/100 MHz** | `MULTITHREADING-PLAN.md` §2; `ml505top.v` |
| CPU stalls only on cache miss (`Cache.v` `stall`, `stall_top`) | same; `Cache.v:222` `stall = (ns != IDLE)` |
| D$/I$: direct-mapped, **256 lines × 32 B**; write-through, no write-allocate; fill on miss | `cache.vh`; `QUIRKS.md` dcache-stale |
| Arbiter: one DDR beat at a time; priority I$ > D$ > GPU… | `RequestController.v`; multithreading plan |
| CNT_CYCLE counts **clocks including stalls**; vvizard today uses insn count for both CNT_* | `QUIRKS.md` “Differences that are not emulated” |
| vvizard today: **no stall or cache timing** (1 cycle/insn) | `README.md` Counters / hardware model |

MIG-ish timing baked into Coregen DDR2 (not a guest-facing number; informs approx):
`CAS_LAT`, `TRCD=15000` ps, `TRP=15000` ps, `CLK_PERIOD=5000` ps (200 MHz PHY) in `ddr2_phy_top.v` / `ddr2_ctrl.v`.

CoreHarness (CPU-only Icarus, **no MIG**) approximates DDR **stores** as a fixed `NSTALL` (default **4**) CPU-cycle stall on writes to `0x1xxxxxxx` (`hardware/sim-icarus/tb/CoreHarness.v`). Used by `cpu_isa_ddr` (`NSTALL=4`) and `isr_epc_stall{4,12}`.

## Xilinx sim sources (same place the project already documents)

**Do not invent a new path.** Use cs150 `hardware/sim-icarus` as documented in:

- `hardware/sim-icarus/README.md` (“Xilinx simulation sources”)
- `hardware/sim-icarus/Makefile` (`XILINX_SRC`, `ISE_SUBDIR := 14.7/ISE_DS/ISE/verilog/src`)

Compile flags for tests with `.NEEDS := xilinx`:
`-y $XILINX_SRC/unisims -y $XILINX_SRC/XilinxCoreLib` plus `$XILINX_SRC/glbl.v`.

Auto-detect order (first hit wins): `$XILINX/verilog/src`, `$ISE_DOCKER_DIR/opt/Xilinx/14.7/...`, `/tmp/vmfs/opt/Xilinx/14.7/...`, `~/ise-docker/opt/Xilinx/14.7/ISE_DS/ISE/verilog/src`, `/workspace/ise-docker/...`, `/opt/Xilinx/14.7/...`, `~/opt/xilinx-verilog/src`.

On UBU24 (2026-11-05): **`XILINX_SRC=/home/ejr/ise-docker/opt/Xilinx/14.7/ISE_DS/ISE/verilog/src`** (`make env` → `HAVE=xilinx mips coregen`).

## Icarus measurements (UBU24, cs150 `0ff61b6`, no Verilog edits)

### A. CoreHarness fixed-NSTALL (no Xilinx)

| Test | Result | Notes |
|---|---|---|
| `isr_epc_nostall` | PASS | NSTALL=0; main progress=2705 in fixed sim window |
| `isr_epc_stall4` | PASS | NSTALL=4; main progress=1503 (~44% of nostall) |
| `isr_epc_stall12` | PASS | NSTALL=12; main progress=910 |
| `cpu_isa_ddr` | PASS | **521 cycles, 20 stalled, NSTALL=4**; 69 checks |

Interpretation: harness store-stall of **4 cycles** is a deliberate approximation for “DDR store”, not MIG physics.

### B. Real Cache + MIG + Micron DDR2 (`make cache_tb`, needs Xilinx+coregen)

`CacheTestBench` reports CPU-cycle counts while `stall` is high (`cachetesttasks.vh`, `MAX_STALLS=50`):

| Op | Cycles (CPU @ 50 MHz) |
|---|---|
| D-write (write-through) | **3** |
| D-read miss (cold fill @ 0) | **16** |
| D-read hit | **1** |
| D-read miss @ 0x00100000 | **23** |
| D-read (later, conflict/refill) | **26** |

All tests PASSED. These are the best current **approx miss/hit stall** numbers for a vvizard DDR-delay mode (not claimed as cycle-accurate for every arbiter/GPU contention case).

## Proposed emulator shape (next commits; not this one)

1. Flag off by default; `--cpu=fast` unchanged when flag off.
2. When on: model I$ and D$ like `Cache.v`/`cache.vh` (256×32B, WT no-WA); on miss add ~**16–26** cycles (start with a single tunable, e.g. `DDR_MISS_CYCLES=20`, refine from more Icarus runs); write-through ~**3** cycles.
3. Advance `vv_hw_tick` / COP0 Count / CNT_* by **stall cycles** when mode on (closes the QUIRKS gap).
4. Keep GPU busy model separate; do not require full RequestController in v1.
5. Tests: differential vs Unicorn only with mode off; new cases vs Icarus `cpu_isa_ddr` / documented cycle bands with mode on; SKIP if no CS150_ROOT/Icarus.

## Non-goals (near term)

- Full MIG timing / multi-master arbitration fidelity.
- Changing default backend away from fast.
- Editing cs150 Verilog or `proj7.c`.

## Implementation status

Landed on this branch as `--ddr-stalls` (default off):

- `src/memstall.c`: I$/D$ tag arrays (256×32B), miss=`VV_MS_MISS_CYCLES` (20), write=`VV_MS_WRITE_CYCLES` (3)
- Gated on `vv_ddr_stalls`; fast/Unicorn paths call `vv_ms_*` only when set (early out)
- `vv_hw_tick` / CNT_CYCLE use `vv_ms_cycles()` when on so cycles can exceed insn count
- `[ddr]` summary line; `tests/test_ddr_stalls.py` + `tests/ddr_stall.s`
