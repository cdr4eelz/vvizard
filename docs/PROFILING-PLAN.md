# Profiling plan

This is a plan. Nothing in it is implemented yet. The default `make` stays as it is: `-O3 -g` (`OPTFLAGS` in the `Makefile`), with no profiler linked and no counters compiled in.

Baseline: about 10.7M guest instructions/s on Linux at `-O3`, and slower on the macOS (Apple Silicon) MacBook. The README "Cost" and NOTES "Speed" sections measured an `-O0` build (GIOS+proj7, 20M instructions, headless): 4.4 s with `--no-quirks` and 5.8 s with the default quirks. Profile the `-O3` build.

## 1. Tool: samply + built-in counters

- **samply** is a sampling profiler that runs on Apple Silicon macOS and on Linux. It uses the same command on both (`samply record ./vvizard …`) and opens the result in the Firefox Profiler UI. On Linux it reads `perf_event_open`; on macOS it reads the Mach task APIs and needs no Xcode. It shows where the time goes, including inside `libunicorn`, without changing the code.
- **Counters and timers** in `src/profile.h`, compiled in only with `make PROFILE=1`. At `-O3`, `vv_hw_tick`, `vv_quirk_pre` and `irq_step` get inlined into `hook_code`, so sampling blurs them. The counters give exact call counts and the time spent in each callback.
- **Not chosen:**
  - gprof: Apple clang has no `-pg`, and gprof cannot see inside the prebuilt `libunicorn`.
  - gperftools/pprof: it works on both systems, but it has to be linked in or preloaded, and its stack unwinding is weaker than samply's.
  - Tracy: it needs a C++ client and server, and a timing zone per instruction would cost more than the code being measured.
  - perf (Linux) and Instruments (macOS): good fallbacks, but two different UIs make a Mac-vs-Linux comparison harder. Commands are in section 4.

## 2. Implementation (off by default)

**Makefile:** add `PROFILE ?= 0`. With `PROFILE=1`, append `-fno-omit-frame-pointer -DVV_PROFILE` to `CFLAGS` and keep `-O3 -g`. `FLAGS_LINE` already includes `CFLAGS`, so switching the flag rebuilds everything. No new library is needed.

**`src/profile.h`:** when `VV_PROFILE` is unset, every macro expands to nothing. When it is set:
- Counters are plain `uint64_t` slots. All hooks run on the thread that calls `uc_emu_start`, so no atomics are needed.
- `VV_PROF_COUNT(slot)` counts calls. `VV_PROF_T0` / `VV_PROF_T1(slot)` measure elapsed nanoseconds with `clock_gettime(CLOCK_MONOTONIC)`, the clock `now()` in `src/display.c` already uses.
- Timing is skipped unless `--profile` is passed. An instruction takes about 100 ns, so per-instruction timers would distort the result. Time only the coarse slots, and use counts or A/B runs for the per-instruction ones. Calibrate once at startup (1e6 empty timer pairs) and print the cost per pair.
- `vv_prof_report()` prints through `vv_eprintf`, after `vv_hw_summary()` / `vv_quirk_summary()` in `main`. Stdout stays guest UART only.

**Options, only when built with `VV_PROFILE`:** add them to whatever option parser `main` uses at the time.
- `--profile`: print the report at exit (counts, ns per slot, and guest instructions/s = `insn_count` ÷ the `uc_emu_start` wall time).
- `--profile-bare-hook`: `hook_code` only does `insn_count++` and returns. Emulation is wrong in this mode, so print a warning. Use it only to measure timing.
- `--profile-no-mem-unmapped`: skip installing `UC_HOOK_MEM_UNMAPPED`. This is an A/B test for culprit 2 below. Use it only on a guest that never makes an unmapped access.

| slot | where | count or time |
|---|---|---|
| `emu` | `main`, around `uc_emu_start` | time |
| `hook_code` | `hook_code` (`src/main.c`) | count |
| `quirk_pre`, `quirk_post`, `post_fix`, `fetch` | `src/quirk.c` | count |
| `reg_rd`, `reg_wr`, `mem_rd` | `gr`/`sr`/`uc_mem_read` wrappers in `src/quirk.c`, `read_insn` in `src/main.c` | count |
| `dc_load`, `dc_store` | `vv_dc_load`, `vv_dc_store` (`src/dcache.c`) | count |
| `mmio_r`, `mmio_w` | `mmio_read`, `mmio_write` (`src/main.c`) | count |
| `tlb_fill`, `intr`, `unmapped`, `bios_w` | `tlb_fill`, `hook_intr`, `hook_unmapped`, `hook_bios_write` | count |
| `disp_tick`, `poll` | `vv_disp_tick`, `PollInputEvents` branch (`src/display.c`) | count + time |
| `draw`, `grab`, `strip`, `end_draw` | `draw`, `grab`, `draw_strip`, `EndDrawing` (`src/display.c`) | count + time |
| `pf_draw` | `vv_disp_pf_write` called from `vv_hw_tick` (`src/hw.c`) | count |
| `gpu` | `vv_gpu_run` (`src/gpu.c`) | count + time |

## 3. Suspected culprits, verified in the source

`main` (`src/main.c`) installs five hooks, plus MMIO through `uc_mmio_map`. There is no `UC_HOOK_BLOCK` and no `UC_HOOK_MEM_READ`.

```
UC_HOOK_TLB_FILL      tlb_fill         1, 0                       (whole space)
UC_HOOK_CODE          hook_code        1, 0                       (whole space: every instruction)
UC_HOOK_INTR          hook_intr        1, 0                       (CPU exceptions only)
UC_HOOK_MEM_UNMAPPED  hook_unmapped    1, 0                       (whole space)
UC_HOOK_MEM_WRITE     hook_bios_write  0x40000000, 0x4FFFFFFF     (BIOS nibble)
```

In Unicorn, a range with `begin > end` means the whole address space.

1. **`hook_code` runs on every instruction.** Unicorn emits a helper call and syncs the PC before each instruction. The default path through the hook is:
   - `vv_hw_tick` (`src/hw.c`) runs every time.
   - Because `vv_quirk_active()` is set by default (the guest-bug detectors `vv_quirk_bugs()` are on even with `--no-quirks`), it then runs `vv_quirk_post`, `irq_step` and `vv_quirk_pre`.
   - `vv_quirk_pre` calls `fetch`, which does a 4-byte `uc_mem_read` of the instruction on every step. It also does `uc_reg_read` calls through `gr` (for example `addi` under the `overflow` quirk, and stores into the D-cache path) and, for loads, `vv_dc_load` plus possibly another `uc_mem_read`.
   - Every load with a nonzero `rt` sets `P.active`, so the next instruction also runs `post_fix`, which calls `gr` again.
   - The cheapest path, "one branch" per the README, needs both quirk masks empty: `--no-quirks --quiet-quirk=dcache-stale,tx-busy,rx-overrun,irq-storm,undef-reg`. Even then `irq_step` still runs every instruction unless `--no-irq` is passed.
2. **`UC_HOOK_MEM_UNMAPPED` over the whole space probably sends every guest store through Unicorn's slow path.** I checked the current upstream Unicorn source, not the 2.1.x tag that is actually installed:
   - `uc_mem_hook_installed()` (`qemu/include/exec/exec-all.h`) returns true for any page covered by a `*_UNMAPPED` or `MEM_WRITE` hook.
   - In that case `notdirty_write` (`cputlb.c`) never calls `tlb_set_dirty`, so stores keep taking `store_helper` and walking the hook lists.
   - `hook_bios_write` has the same effect, but only on BIOS pages.
   - The A/B test is `--profile-no-mem-unmapped`. If it confirms the cost, narrow the hook to the undecoded or unmapped ranges in `src/memmap.h`.
3. **The `uc_emu_start` count limit adds a second per-instruction hook.** `main` passes `max_insns` as the count. Unicorn's `uc_emu_start` (`uc.c`) then installs its own whole-space `UC_HOOK_CODE` (`hook_count_cb`) whenever count > 0.
   - Normal runs leave `--max-insns` at its default of 0 (no limit), so they don't pay for it. A benchmark run with `--max-insns N` does.
   - For benchmarks, stop with `--until ADDR` (the stop address), or measure the difference between the two once.
4. **Window and status strip: probably not the bottleneck.** The strip is not redrawn per instruction. On every 4096th instruction, `hook_code` calls `vv_term_tick` and then `vv_disp_tick` (`src/display.c`).
   - `vv_disp_tick` calls `now()`. At most every 1/60 s it redraws with `draw`, which runs `grab` (600 `uc_mem_read`s of 3200 B), `UpdateTexture`, `draw_strip` (about 1280 `DrawRectangle`s for the LCD plus a few `DrawText`s), and `EndDrawing`.
   - Otherwise it calls `PollInputEvents()`. At 10.7M instructions/s that is about 2,600 GLFW event polls per second. On macOS each poll goes through the Cocoa run loop, so this poll is a more likely cost than the strip drawing.
   - Also measure `EndDrawing` on its own, because a swap that waits for vsync or compositing on macOS would stall emulation.
   - `vv_hw_tick` also calls `draw` through `vv_disp_pf_write` on every PixelFeeder frame change (`VV_PF_PERIOD` = 865800 cycles, about 12 per second).
   - If the poll turns out to be expensive, throttle it by time instead of by instruction count.
5. **Less likely, but count them:**
   - `tlb_fill` runs only on Unicorn TLB misses, which are filled per page.
   - `mmio_read` / `mmio_write` run only for the 16 KiB MMIO page. Logging is off by default; with `--mmio-log` you are measuring the logging.
   - `vv_gpu_run`: one GP command can fill all 800×600 pixels.
   - `vv_hw_tick` calls `poll()` on stdin at most every 4096 cycles.
6. **macOS-specific checks before profiling:**
   - Run `file ./vvizard` and `file $(otool -L ./vvizard | awk '/unicorn/{print $1}')`. Both must say `arm64`. An x86_64 Homebrew or Unicorn build running under Rosetta would explain a large slowdown by itself.
   - Unicorn has no interpreter (TCI) build option, and on Apple Silicon it uses its native aarch64 JIT with `MAP_JIT`. If samply shows a lot of time in `tb_gen_code` / `tcg_gen_code` or in JIT write-protect toggling, the translated code is being thrown away and rebuilt (translation-block churn). The `uc_mem_write` calls in `post_fix` are one possible cause.

### Order of runs

Use the same guest and fixed stop point on each run (`--until` or a fixed `--max-insns`, the same on every machine), with `--no-window` and no `--mmio-log`. Run the window case last.

| run | flags added to `--profile` | isolates |
|---|---|---|
| A | (default quirks) | full cost |
| B | `--profile-bare-hook` | cost of Unicorn calling a hook on every instruction |
| C | `--no-quirks --quiet-quirk=dcache-stale,tx-busy,rx-overrun,irq-storm,undef-reg` | quirk checks (A − C) |
| D | C + `--no-irq` | `irq_step` |
| E | A + `--profile-no-mem-unmapped` | slow store path (culprit 2) |
| F | A with the window on | display, poll and `EndDrawing` |
| G | A with `--max-insns 0 --until ADDR` instead of `--max-insns N` | Unicorn's count hook (culprit 3) |

Take samply profiles of A and F on both operating systems. Counters are enough for the others. Compare instructions/s for Mac vs. Linux run by run. A gap that is already large in B points at Unicorn or the host, not at vvizard's hook bodies.

## 4. Install and run

With no arguments, `./vvizard` runs the built-in defaults: GIOS, the proj7 player and the autorun command. A profiling run only adds a few flags:

```
RUN="./vvizard --no-window --random-ddr=1 --max-insns 20000000 --profile"
```

Run the binary directly under samply. `samply record make run` would profile `make`.

### macOS (Apple Silicon)

```
brew install samply            # Homebrew core formula
make PROFILE=1
eval samply record $RUN        # opens the Firefox Profiler in a browser when the run exits
```

- There is no need to set `LD_LIBRARY_PATH`: the Makefile links with `-Wl,-rpath,$HOME/.local/lib`, which macOS follows.
- `samply setup` is needed only for attaching to a running process (`samply record -p PID`). Launching a fresh, unsigned `make` build works without it.
- Fallback: Instruments, `xcrun xctrace record --template 'Time Profiler' --launch -- ./vvizard …`. This needs Xcode.

### Linux (Ubuntu 24.04, Debian 13)

samply is not packaged in apt. Use the upstream installer, `cargo install --locked samply`, or Linuxbrew:

```
curl --proto '=https' --tlsv1.2 -LsSf https://github.com/mstange/samply/releases/latest/download/samply-installer.sh | sh
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid    # allow unprivileged perf events
make PROFILE=1
eval LD_LIBRARY_PATH=$HOME/.local/lib samply record $RUN
```

Fallback, perf:
- Install it with `sudo apt install linux-tools-generic` on Ubuntu 24.04, plus `linux-tools-$(uname -r)` if the versions don't match, or `sudo apt install linux-perf` on Debian 13.
- Then run `perf stat -e cycles,instructions -- ./vvizard …`, `perf record --call-graph fp -- ./vvizard …` and `perf report`.

Unicorn functions appear only as addresses unless `libunicorn` was built with symbols (`-DCMAKE_BUILD_TYPE=RelWithDebInfo`). Rebuild it that way in `~/.local` on both machines so the profiles name Unicorn's functions.
