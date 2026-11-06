# Implementation report

Branch `feat/custom-mips-backend`. Implementation commit `c0329201f114b0aea72cbceb927b4c60eff41d21` (base `731faab48f947bfc3f122294812791e06fc0deed`). Unicorn stays the default CPU. `--cpu=fast` is optional. `--cpu=rtl` exists only in a `make FIDELITY=rtl` build.

This host is Debian 13, x86_64, Unicorn 2.1.1, raylib. `LD_LIBRARY_PATH=$HOME/.local/lib`. Numbers below are MEASURED on this machine unless marked ESTIMATE. No other host was available, so the plan's two-of-three-hosts bar was not run.

## Decision

Do not flip the default.

`--cpu=fast` is faster than Unicorn on the corpora that could be run here. The straight ALU loop is 1.509× on guest instructions/s. The two-instruction branch loop is 1.343×. Headless wall-clock instructions/frame are 1.438×. Windowed instructions/frame are 1.277×. Both CPUs still finish a wall-clock run at about 60.2 frames/s, because the frame period is 16.5792 ms. The frames/s ratio is 0.999×.

The plan keeps the fast engine optional when a guest result is between 1.15× and 1.5×, and it refuses a default flip when end-to-end throughput is under 1.10×. The frames/s ratio is under that line. The 1.5× guest bar was also specified for at least two of three hosts. Only this host was measured, and the branch loop is under 1.5×. The proj7 player images are not in this tree, so that suite did not byte-match.

An earlier fast build, before quiet spans were allowed while IE was clear and before a span could cross a branch, was slower than Unicorn on these same loops (about 0.83× to 0.91×). Those runs are superseded. They are not the gate.

## What was added

| Path | Role |
|---|---|
| `src/cpu/fast/` | Predecoded basic-block interpreter (`--cpu=fast`) |
| `src/cpu/irq.c` | Shared interrupt entry for Unicorn and fast |
| `src/cpu/rtl/rtl.c` | Approximate I-cache and D-cache stalls (`--cpu=rtl`) |
| `src/cpu/service.h`, `src/cpu/backend.h` | Service and backend hooks |
| `src/profile.c`, `src/profile.h` | Counters from `docs/PROFILING-PLAN.md` |
| `src/main.c`, `src/hw.c`, `src/quirk.c` | `--cpu`, quiet budget, shared quirk helpers |
| `Makefile` | `PROFILE=1`, `FIDELITY=rtl` |

`make` does not pass `-DVV_FIDELITY_RTL` and does not link `rtl.o`. After the final `make -j4 PROFILE=0 FIDELITY=l0`, `nm vvizard` has no `vv_rtl_run` and no `icache`. The data symbol `vv_rtl_cache` is only the CLI flag. `CNT_CYCLE` and the PRNG use the instruction count in that binary. Under `FIDELITY=rtl` they use `vv_hw_now()`.

The functional D-cache in `src/dcache.c` is unchanged except for two profile counts. It is not the RTL tag array.

Intentional shared7 mismatch is untouched: C `FEDBEEF3` / major 7 in `vendor/cs150/software/proj_app7/shared7.h`, ASM `FEDBEEF2` / major 6 in `shared7.s.inc`. No edits under `tool/vizard`, `hardware/`, or `proj7.c`.

## How to build and run

```
make -j4                         # Unicorn default, no cache-stall code
make -j4 FIDELITY=rtl            # also --cpu=rtl
make -j4 PROFILE=1               # counters; run with --profile
```

```
./vvizard --cpu=fast --no-window --sync ...
./vvizard --cpu=rtl --rtl-cache=on|off ...    # only after FIDELITY=rtl
```

`--cpu` defaults to `unicorn`. `VVIZARD_CPU` is used when `--cpu` is absent. `--cpu=fast` and `--cpu=rtl` reject `--little` (exit 2). `--cpu=rtl` on a default build exits 2 with `make FIDELITY=rtl`. Do not export `VVIZARD_CPU` for `make test`. `tests/compare_runs.py` clears `VVIZARD_QUIRKS` and leaves `VVIZARD_CPU` in the environment.

`make test` does not include `make test-fast`. `test-fast` is `compare_runs.py` of `--cpu=unicorn --sync` against `--cpu=fast --sync`.

## Correctness

MEASURED on the L0 binary:

- Unicorn, after the quiet-budget change: `python3 tests/test_hex.py` 8/8, `tests/test_hw.py` 23/23, `tests/test_quirks.py` 55/55. Exit 0.
- `--cpu=fast` (`VVIZARD_CPU=fast`), same three commands, after block chaining: 8/8, 23/23, 55/55. Exit 0.
- After the fast path publishes the cycle count before `vv_irq_step`: `python3 tests/compare_runs.py "./vvizard --cpu=unicorn --sync" "./vvizard --cpu=fast --sync" --insns 2000000` exited 1 with 31/33 SAME. DIFF only `proj7 headless` (`f.ppm`) and `proj7 keys` (`s40.ppm`, `s60.ppm`), 0 UART bytes. Both sides miss the guest images.

`../../software/gios/gios.inst.bin` is present. These are not:

- `../../software/bench/bench.inst.bin`
- `../../software/proj_app7/isr7.bin`
- `../../software/proj_app7/proj7.inst.bin`

`make test`'s player, shared-memory part C, and threads player targets need those files. They were not treated as a CPU failure, and no frames/s number for the proj7 player is reported. `make` prints `vendor-cmp: all 5 vendored cs150 headers match`.

`hw/irq_defer` event traces matched only after quiet instructions stored the instruction count into the cycle clock before sampling the interrupt. The entry PC was already the same. The trace prefix is the cycle.

Known gap: `beql` / `bnel` / `blezl` / `bgtzl` decode as real branch-likely when the reserved quirk is off, and as reserved nops when it is on. A full `--quirks=none` `compare_runs` was not re-run. `ll` / `sc` / COP1 and other opcodes at or above `0x18` stay reserved-illegal when that quirk is off. Unicorn may execute some of them.

## Throughput

Protocol: `taskset -c 2`, one warmup discarded (`--max-insns 5000000` or `--max-seconds 2`), then five runs, median of the sorted runs. Seconds are the `[frame]` wall time. The gate binary is `make -j4 PROFILE=0 FIDELITY=l0`, not the `PROFILE=1` build.

Common flags: `--no-window --random-ddr=12345 --no-halt-on-quirk --no-bios --sync` plus the image below. Threads rows use `--threads=on` instead of `--sync`. Emulated rows add `--frame-clock=emulated --max-insns 50000000`. Wall rows add `--frame-clock=wall --max-seconds 10`.

Images, both at `0x40000000` (BIOS, not a cached DDR nibble):

- Spin: `beq $zero,$zero,-1; nop`.
- ALU: sixteen `addiu $t0,$zero,1`, then `beq $zero,$zero,-17; nop`.

On L0, `[hw] cycles` equals the instruction count. Emulated 50M runs scanned 60 frames. Exit code 0 on every run in the table.

| Case | Unicorn median (range) | Fast median (range) | Fast/Unicorn |
|---|---|---|---|
| Spin, default quirks, 50M | 1.437 s, 34.795 M/s (1.426–1.461) | 1.070 s, 46.729 M/s (1.036–1.077) | 1.343× |
| Spin, `--quirks=none`, 50M | 1.518 s, 32.938 M/s (1.511–1.535) | 1.102 s, 45.372 M/s (1.093–1.130) | 1.377× |
| ALU, default quirks, 50M | 1.435 s, 34.843 M/s (1.402–1.453) | 0.951 s, 52.576 M/s (0.948–0.960) | 1.509× |
| ALU, `--threads=on`, 50M | 1.418 s, 35.261 M/s (1.409–1.459) | 0.956 s, 52.301 M/s (0.952–0.981) | 1.483× |
| ALU, headless wall 10 s | 355258368 insns, 603 frames, 10.000 s, 35.526 M/s, 589152 insns/frame, 60.300 frames/s | 510033920 insns, 602 frames, 9.996 s, 51.024 M/s, 847232 insns/frame, 60.224 frames/s | 1.436× insns/s, 1.438× insns/frame, 0.999× frames/s |
| ALU, windowed wall 10 s, `DISPLAY=:2`, `--window --sync` | 244170752 insns, 603 frames, 10.002 s, 24.412 M/s, 404927 insns/frame, 60.288 frames/s | 311193600 insns, 602 frames, 9.997 s, 31.129 M/s, 516933 insns/frame, 60.218 frames/s | 1.275× insns/s, 1.277× insns/frame, 0.999× frames/s |

Unicorn headless wall insns ranged 354258944–357429248, all 10.000 s and 603 frames. Fast headless insns ranged 501198848–519888896 over 9.995–9.997 s, all 602 frames. Windowed Unicorn insns ranged 239087616–245858304. Windowed fast insns ranged 303611904–330940416.

The windowed rows are the BIOS ALU loop, not the proj7 player. A player frames/s number is not invented.

A discarded first attempt wrote the spin image with shell `printf '\x..'`, which stored the ASCII bytes of that text. Those rates are not used.

## Profile

`make PROFILE=1` and `--profile`, one 5M ALU run, not the gate binary (frame pointers are on). Counts:

- Unicorn: `hook_code` 5000001, `quirk_pre` / `quirk_post` / `hw_tick` / `irq` 5000000, `tlb_fill` 2. Emu wall 165.848 ms. Hook shares print about 99% because the timers nest.
- Fast: `hook_code` 1366, `hw_tick` 1365, `irq` 5000000, `fast_insn` 5000000, quirk counts 0. Emu wall 109.593 ms. The printed irq share of `hook_code` is far above 100% because `vv_irq_step` runs on quiet instructions that are not hook calls. The counts are the reading. `vv_hw_tick` is no longer per instruction. `vv_irq_step` still is.

Profile percentages were not used to accept or reject the prototype. The throughput table was.

## RTL cache model

APPROXIMATE, not cycle-exact. `Cache.v` is not in this snapshot. The model follows:

- `cpu/MemBank.v`: fetch and data nibbles 0x1, 0x2, and 0x3 use the caches. BIOS 0x4, D-scratch 0x5, I-scratch 0x6, and ISR 0xC are BRAM. An MMIO fetch stalls 0.
- `cache.vh` geometry, already used by `src/dcache.h`: direct-mapped, 256 lines × 32 bytes, index `addr[12:5]`, tag `addr[27:13]`. Separate I and D tag arrays, only in `rtl.c`.
- Icarus `CPU_prog_tb.v` `NSTALL` default 4. `MIPS150.v` COP0 `.ena(1'b1)`, so Count advances on a stall.

A cacheable load or fetch costs 4 on a miss and then fills. A second load of the same line costs 0. A committed store to a cacheable DDR nibble costs 4 even on a hit, does not allocate, and does not snoop the I-cache. Dropped stores cost 0. An I-miss plus a D-miss on one instruction costs 8. IRQ is not sampled during stall cycles. `--rtl-cache=off` forces cycles equal to instructions. The D-cache address is architectural base+offset (`rs`), not `rt`.

MEASURED with `make -j4 FIDELITY=rtl`, then the L0 binary was restored:

| Program | Mode | Cycles | Insns |
|---|---|---|---|
| BIOS spin, 44 insns | `--cpu=rtl` cache on | 44 | 44 |
| Same image at `0x10000000` | cache on | 48 | 44 |
| Same DDR image | `--rtl-cache=off` | 44 | 44 |
| Same DDR image | `--cpu=fast` on the RTL binary | 44 | 44, no `rtl cycles` suffix |
| `lui $t0,0x1000; lw $t1,0($t0); lw $t2,4($t0); break` | cache on | 8 | 4 |
| `lui; lw $t1,0; lw $t2,32($t0); break` | cache on | 12 | 4 |
| `lui; sw $t0,0($t0); break` | cache on | 7 | 3 |

## Limitations

- The fast loop still calls `vv_irq_step` once per instruction so deferral and IE edges stay aligned with Unicorn. That is the remaining per-instruction service on a quiet ALU run.
- Blocks are at most 64 instructions, and a delay slot may cross a page. MMIO, `mfc0` / `mtc0`, `eret`, `syscall`, and `break` return to a real `vv_hw_tick` before they run.
- `--quirks=none` was measured for speed on the spin loop and was not fully differentially compared.
- Division by zero in the fast HI/LO helper was not given its own test.
- Cycle-exact simulation is out of scope. Extra stall cycles spend the frame budget on non-instructions. The product path stays 1 CPI.
- Windowed and headless frames/s will not show a CPU win while both sides keep up with the 60.3 Hz wall clock. Instructions per frame is the CPU comparison in that mode.
