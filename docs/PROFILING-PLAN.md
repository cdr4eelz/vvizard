# Profiling plan

This is a plan. Nothing in it is implemented yet. The default `make` stays as it is: `-O3 -g` (`OPTFLAGS` in the `Makefile`), with no profiler linked and no counters compiled in.

It covers the current single-threaded build and the threaded design in `docs/MULTITHREADING-PLAN.md` (`--sync` keeps today's one thread). Take a baseline (runs A–G below, and on macOS especially F for `end_draw` and `poll`) before that split. The macOS profile decides whether the split is worth doing there.

Baseline: about 10.7M guest instructions/s on Linux at `-O3`, and slower on the macOS (Apple Silicon) MacBook. The README "Cost" and NOTES "Speed" sections measured an `-O0` build (GIOS+proj7, 20M instructions, headless): 4.4 s with `--no-quirks` and 5.8 s with the default quirks. Profile the `-O3` build.

## 1. Tool: samply + built-in counters

- **samply** is a sampling profiler that runs on Apple Silicon macOS and on Linux. It uses the same command on both (`samply record ./vvizard …`) and opens the result in the Firefox Profiler UI. On Linux it reads `perf_event_open`; on macOS it reads the Mach task APIs and needs no Xcode. It shows where the time goes, including inside `libunicorn`, without changing the code. It records every thread. In the Firefox Profiler, use the per-thread tracks. Name the threads so the tracks are labeled: Linux `pthread_setname_np(pthread_self(), "vv-emu")` (the name must be ≤15 characters); macOS `pthread_setname_np("vv-emu")`, which names only the calling thread. Use `vv-emu`, `vv-gpu`, and `vv-main`. On today's build the emulator and the window share one track, so run F shows `end_draw` and `poll` there. After the split, check `vv-emu` for stalls: `EndDrawing` or `PollInputEvents` there means the window work is still on the guest path.
- **Counters and timers** in `src/profile.h`, compiled in only with `make PROFILE=1`. At `-O3`, `vv_hw_tick`, `vv_quirk_pre` and `irq_step` get inlined into `hook_code`, so sampling blurs them. The counters give exact call counts and the time spent in each callback.
- **Not chosen:**
  - gprof: Apple clang has no `-pg`, and gprof cannot see inside the prebuilt `libunicorn`.
  - gperftools/pprof: it works on both systems, but it has to be linked in or preloaded, and its stack unwinding is weaker than samply's.
  - Tracy: it needs a C++ client and server, and a timing zone per instruction would cost more than the code being measured.
  - perf (Linux) and Instruments (macOS): good fallbacks, but two different UIs make a Mac-vs-Linux comparison harder. Commands are in section 4.

## 2. Implementation (off by default)

**Makefile:** add `PROFILE ?= 0`. With `PROFILE=1`, append `-fno-omit-frame-pointer -DVV_PROFILE` to `CFLAGS` and keep `-O3 -g`. `FLAGS_LINE` already includes `CFLAGS`, so switching the flag rebuilds everything. No new library is needed.

**`src/profile.h`:** when `VV_PROFILE` is unset, every macro expands to nothing. When it is set:
- Counters are plain `uint64_t` slots, one struct per thread (main/display, emulator, GPU worker): `_Thread_local`, or a struct passed into that thread. Each thread writes only its own struct, so no atomics are needed. `vv_prof_report()` merges them after the workers have joined.
- `VV_PROF_COUNT(slot)` counts calls. `VV_PROF_T0` / `VV_PROF_T1(slot)` measure elapsed nanoseconds with `clock_gettime(CLOCK_MONOTONIC)`, the clock `now()` in `src/display.c` already uses.
- Timing is skipped unless `--profile` is passed. An instruction takes about 100 ns, so per-instruction timers would distort the result. Time only the coarse slots, and use counts or A/B runs for the per-instruction ones. Calibrate once at startup (1e6 empty timer pairs) and print the cost per pair.
- `vv_prof_report()` prints through `vv_eprintf`, after `vv_hw_summary()` / `vv_quirk_summary()` in `main`. Stdout stays guest UART only.
- Also print wall time against per-thread CPU time. Each thread samples `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` across its own loop; `getrusage(RUSAGE_THREAD, …)` on Linux is the per-thread cross-check (`RUSAGE_SELF` is the process total on both). The number that matters is the emulator thread's busy fraction (its CPU time ÷ wall): that thread is the critical path. Also report main-thread idle and how long the GPU worker blocked on its semaphore. For the `vv_eprintf` mutex, `trylock` first and time `pthread_mutex_lock` only when that fails; the slot is the time spent waiting, not the time holding the lock.

**Options, only when built with `VV_PROFILE`:** add them to whatever option parser `main` uses at the time.
- `--profile`: print the report at exit (counts, ns per slot, and guest instructions/s = `insn_count` ÷ the wall time of `uc_emu_start`, measured on the thread that calls it; not the emulator thread's CPU time, and not including loading or `--random-ddr`).
- `--profile-bare-hook`: `hook_code` only does `insn_count++` and returns. Emulation is wrong in this mode, so print a warning. Use it only to measure timing.
- `--profile-no-mem-unmapped`: skip installing `UC_HOOK_MEM_UNMAPPED`. This is an A/B test for culprit 2 below. Use it only on a guest that never makes an unmapped access.

The `thread` column is who writes the slot after the split. Under `--sync` there is one thread and it owns every slot: `grab` is inside `draw`, and `frame_copy`, `swizzle`, `upload`, `tb_*`, `gpu_wait`, and `gp_busy` stay at zero.

| slot | thread | where | count or time |
|---|---|---|---|
| `emu` | emu | around `uc_emu_start` (wall), on the emulator thread | time |
| `hook_code` | emu | `hook_code` (`src/main.c`) | count |
| `quirk_pre`, `quirk_post`, `post_fix`, `fetch` | emu | `src/quirk.c` | count |
| `reg_rd`, `reg_wr`, `mem_rd` | emu | `gr`/`sr`/`uc_mem_read` wrappers in `src/quirk.c`, `read_insn` in `src/main.c` | count |
| `dc_load`, `dc_store` | emu | `vv_dc_load`, `vv_dc_store` (`src/dcache.c`) | count |
| `mmio_r`, `mmio_w` | emu | `mmio_read`, `mmio_write` (`src/main.c`) | count |
| `tlb_fill`, `intr`, `unmapped`, `bios_w` | emu | `tlb_fill`, `hook_intr`, `hook_unmapped`, `hook_bios_write` | count |
| `disp_tick`, `poll` | main | `vv_disp_tick`, `PollInputEvents` (`src/display.c`). `--sync`: from `hook_code` | count + time |
| `draw`, `grab`, `strip`, `end_draw` | main | `draw`, `grab`, `draw_strip`, `EndDrawing`. `--sync`: on the emu thread, `grab` included | count + time |
| `pf_draw` | emu | `vv_disp_pf_write` from `vv_hw_tick`. `--sync`: this calls `draw`. Threaded: it only requests a copy | count |
| `frame_copy` | emu | snapshot `memcpy` of the displayed frame (threaded only) | count + time |
| `swizzle`, `upload` | main | byte loop from `grab`, then `UpdateTexture` (threaded only) | count + time |
| `gpu` | gpu | `vv_gpu_run` (`src/gpu.c`). `--sync`: inline on emu. Threaded: worker host time | count + time |
| `gpu_wait` | emu | emulated busy cycles the guest waited, plus launches whose ready edge was late because the worker was not done and those extra cycles | cycles + count |
| `gp_busy` | emu | dropped launches (`gp-busy`) | count |
| `tb_pub`, `tb_skip` | emu | frames published, frames skipped (unpublished slot overwritten) | count |
| `tb_wait` | main | times main blocked for a slot (should stay ~0; the emu thread must not wait) | count + time |
| `eprintf_lock` | any | time waiting on the `vv_eprintf` mutex (timed only when `trylock` fails) | time |

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
   - The cheapest path, "one branch" per the README, needs both quirk masks empty: `--no-quirks --quiet-quirk=dcache-stale,tx-busy,rx-overrun,irq-storm,undef-reg`. Add `gp-busy` to that list once the detector exists, or it keeps `vv_quirk_active()` set. Even then `irq_step` still runs every instruction unless `--no-irq` is passed.
2. **`UC_HOOK_MEM_UNMAPPED` over the whole space probably sends every guest store through Unicorn's slow path.** I checked the current upstream Unicorn source, not the 2.1.x tag that is actually installed:
   - `uc_mem_hook_installed()` (`qemu/include/exec/exec-all.h`) returns true for any page covered by a `*_UNMAPPED` or `MEM_WRITE` hook.
   - In that case `notdirty_write` (`cputlb.c`) never calls `tlb_set_dirty`, so stores keep taking `store_helper` and walking the hook lists.
   - `hook_bios_write` has the same effect, but only on BIOS pages.
   - The A/B test is `--profile-no-mem-unmapped`. If it confirms the cost, narrow the hook to the undecoded or unmapped ranges in `src/memmap.h`.
3. **The `uc_emu_start` count limit adds a second per-instruction hook.** `main` passes `max_insns` as the count. Unicorn's `uc_emu_start` (`uc.c`) then installs its own whole-space `UC_HOOK_CODE` (`hook_count_cb`) whenever count > 0.
   - Normal runs leave `--max-insns` at its default of 0 (no limit), so they don't pay for it. A benchmark run with `--max-insns N` does.
   - For benchmarks, stop with `--until ADDR` (the stop address), or measure the difference between the two once.
4. **Window and status strip: probably not the bottleneck.** The strip is not redrawn per instruction. This paragraph is the single-threaded build and `--sync`. On every 4096th instruction, `hook_code` calls `vv_term_tick` and then `vv_disp_tick` (`src/display.c`).
   - `vv_disp_tick` calls `now()`. At most every 1/60 s it redraws with `draw`, which runs `grab` (600 `uc_mem_read`s of 3200 B), `UpdateTexture`, `draw_strip` (about 1280 `DrawRectangle`s for the LCD plus a few `DrawText`s), and `EndDrawing`.
   - Otherwise it calls `PollInputEvents()`. At 10.7M instructions/s that is about 2,600 GLFW event polls per second. On macOS each poll goes through the Cocoa run loop, so this poll is a more likely cost than the strip drawing.
   - Also measure `EndDrawing` on its own, because a swap that waits for vsync or compositing on macOS would stall emulation. That stall is on the emulator thread only in this build.
   - `vv_hw_tick` also calls `draw` through `vv_disp_pf_write` on every PixelFeeder frame change (`VV_PF_PERIOD` = 865800 cycles, about 12 per second).
   - If the poll turns out to be expensive, throttle it by time instead of by instruction count.
   - Threaded (`docs/MULTITHREADING-PLAN.md`): on that 4096 boundary `hook_code` still flushes UART via `vv_term_tick` and samples quit, `want_frame`, and GPU done. It does not call `vv_disp_tick`. `vv_disp_pf_write` requests `frame_copy` and does not call `draw`. Swizzle, `UpdateTexture`, `draw_strip`, `EndDrawing`, and `PollInputEvents` run on main. A vsync wait shows up on the `vv-main` track and does not stall the guest; the emulator overwrites an unpublished slot instead of waiting. Run F, and on macOS `end_draw` and `poll`, before the split.
5. **Less likely, but count them:**
   - `tlb_fill` runs only on Unicorn TLB misses, which are filled per page.
   - `mmio_read` / `mmio_write` run only for the 16 KiB MMIO page. Logging is off by default; with `--mmio-log` you are measuring the logging.
   - `vv_gpu_run`: one GP command can fill all 800×600 pixels. Threaded, that host time is the `gpu` slot on the worker. `gpu_wait` is the emulated busy cycles the guest waited, plus the extra cycles on launches whose ready edge waited because the worker was not done.
   - `vv_hw_tick` calls `poll()` on stdin at most every 4096 cycles.
6. **macOS-specific checks before profiling:**
   - Run `file ./vvizard` and `file $(otool -L ./vvizard | awk '/unicorn/{print $1}')`. Both must say `arm64`. An x86_64 Homebrew or Unicorn build running under Rosetta would explain a large slowdown by itself.
   - Unicorn has no interpreter (TCI) build option, and on Apple Silicon it uses its native aarch64 JIT with `MAP_JIT`. If samply shows a lot of time in `tb_gen_code` / `tcg_gen_code` or in JIT write-protect toggling, the translated code is being thrown away and rebuilt (translation-block churn). The `uc_mem_write` calls in `post_fix` are one possible cause.

### Order of runs

Take A–G on the single-threaded build before the split. After it lands, pass `--sync` on A–G so they stay that path (headless default becomes the async GPU). Same guest and the same `--until` or `--max-insns` on every machine, no `--mmio-log`. A–E and G are `--no-window`. Window runs last.

| run | flags added to `--profile` | isolates |
|---|---|---|
| A | (default quirks) | full cost |
| B | `--profile-bare-hook` | cost of Unicorn calling a hook on every instruction |
| C | `--no-quirks --quiet-quirk=dcache-stale,tx-busy,rx-overrun,irq-storm,undef-reg` (add `gp-busy` once that detector exists) | quirk checks (A − C) |
| D | C + `--no-irq` | `irq_step` |
| E | A + `--profile-no-mem-unmapped` | slow store path (culprit 2) |
| F | A with the window on | display, poll and `EndDrawing` |
| G | A with `--max-insns 0 --until ADDR` instead of `--max-insns N` | Unicorn's count hook (culprit 3) |
| H | F + `--sync` | window, one thread (same as F once `--sync` exists) |
| I | F, no `--sync` | threaded window |
| J | A threaded, against A `--sync` | GPU overlap only |

Take samply profiles of A and F on both operating systems before the split, then of H and I after it. Counters are enough for the others. Compare Mac vs. Linux on A–G run by run. A gap that is already large in B points at Unicorn or the host, not at vvizard's hook bodies.

I should come in near headless A on wall time for the same guest progress. J should be a few percent, the GPU overlap only. The guest instruction stream differs after a GPU launch: proj7's `GP_WAIT()` spins for the busy cycles (`software/proj_app7/proj7.c`), so the same `--max-insns` stop covers less drawing than `--sync`. Compare wall time to that stop and guest-visible progress together (PF_FRAME writes, and frames published, per wall second), on the same guest and the same `--max-insns`.

`make TSAN=1` is a correctness check from the multithreading plan, not a profiling run. TSan slows the process by a lot, and it does not instrument Unicorn's JIT stores.

## 4. Install and run

With no arguments, `./vvizard` runs the built-in defaults: GIOS, the proj7 player and the autorun command. A profiling run only adds a few flags. This is run A on today's build; after the split, add `--sync` so it stays that path:

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
- Then run `perf stat -e cycles,instructions -- ./vvizard …`, `perf record -s --call-graph fp -- ./vvizard …` and `perf report --sort comm,tid`. `perf trace` or `perf sched` is optional, for semaphore and lock wakeups.

Unicorn functions appear only as addresses unless `libunicorn` was built with symbols (`-DCMAKE_BUILD_TYPE=RelWithDebInfo`). Rebuild it that way in `~/.local` on both machines so the profiles name Unicorn's functions.
