# Multithreading plan

## Status (Oct 31 2026, branch `multithreading`)

Implemented (steps 1-5 on Linux; macOS not built or measured). `--threads=on` is the default; `--sync`,
`--threads=1` and `--threads=off` keep the old single thread. Where the code differs from the text below:

- **Step 2 was already done.** Every RAM region, DDR included, was a `uc_mem_map_ptr` host buffer before this
  work (`src/main.c`). `VV_PF_PERIOD` is now 828960, not 865800.
- **Completion cycle is fixed at launch, and the emulator waits for the worker.** At `GP_CODE`, the emulator walks
  the list once without storing (`vv_gpu_count`: same `[gpu]` messages and stats as `vv_gpu_run`), so
  `launch + pixels/4 + 32 * commands` is known before the worker starts. At that cycle `vv_hw_tick` calls
  `vv_gpu_wait` if the worker is still drawing, then raises ready and Cause bit 12. Section 6 lets ready wait for
  both instead. Waiting instead makes threaded runs repeatable: with `--frame-clock=emulated`, two threaded runs,
  or a windowed and a headless one, give byte-identical event traces (`--event-trace`), UART output and dumps.
  The worker is normally done long before.
- **Inline draws.** Section 6 asks when the guest's executed DDR is "awkward to name". `hook_code` records the DDR
  pages the guest has run code from, as a range. `tlb_fill` cannot do this: a data access had already filled the TLB
  entry with execute permission. A launch whose 4 MiB frame overlaps that range draws inline with `uc_mem_write`,
  with the same busy time. So does a list that has words in MMIO, which the worker cannot read. If code runs from a
  frame while the worker is drawing it, `uc_ctl_remove_cache` drops that frame's translations at the ready edge.
- **Host readers wait for the worker.** The window copy (`publish`) and PPM dumps (`grab`) call `vv_gpu_settle`
  first, which waits for a worker drawing that frame. They show the finished draw, and the guest does not see the
  wait.
- **Headless** keeps the emulator on the main thread and adds only the GPU worker. **`--legacy-io`** keeps the GPU
  synchronous.
- **Triple buffer:** a lock-free 3-slot exchange (back/middle/front, a fresh bit on the middle index). The emulator
  publishes at every frame end and on the `vv_disp_tick` timer, and once more at the end of the run.
- **`gp-busy`** is a quirk-table entry (default 1, bug 1, like `tx-busy`). `--halt-on-quirk` therefore stops on it
  by default; proj7 never triggers it.
- **Logging:** one recursive mutex in `src/term.c` covers `vv_eprintf` and also `vv_uart_out` /
  `vv_term_tick`, because both share the stdout color/flush state.
- **New:** `--event-trace FILE` and `tests/compare_runs.py` (section 10's trace comparison), and `make test-threads`
  (`tests/test_threads.py`, new `tests/hw/gpstat_async.s`, `dcache_async.s`, `gp_inline.s`).

Checks: `make test` (all `--sync`): 67 hex, 8/8, 20/20, 55/55, 12/12, plus `test-threads` 9/9, on DEB13 and UBU24.
`--sync` against the pre-threads build (00ac75e plus the event trace only) gives byte-identical UART, event trace,
guest-state stderr and frame dumps on all 29 `compare_runs.py` cases, windowed proj7 included. `--sync` against
threaded differs only in the 10 cases that launch the GPU. `make TSAN=1`: no reports on windowed and headless proj7
(emulated and wall) or on `test-threads`, except `gp_inline`. That test runs code from a frame while the worker
draws it, which is the accepted guest race in section 13.

Measured (proj7, `--frame-clock=emulated --max-insns 50000000`, median of 5 interleaved runs; NOTES.md
"Threads"): windowed DEB13 (Xvfb, llvmpipe) 18.56 -> 23.73 M instr/s (1.28x, wall -20%). Windowed UBU24
(Intel GL) 19.72 -> 21.05 (1.07x, wall -3%). Headless 1.04-1.05x. In wall mode (60 fills/s), headless gains 1.12-1.13x
on both machines.

Baseline (NOTES.md "Window cost", Linux `-O3`, Xvfb, proj7 via GIOS, 50M instructions): about 10.7M guest instructions/s headless. A windowed run is about 5.6 s against about 4.6 s headless. The difference is about 1.0 s inside `draw()` (`src/display.c`): about 291 draws at about 2.7 ms, of which `draw_strip` is about 0.45 ms. `hook_code` (`src/main.c`) runs on every instruction and stays serial. GPU time is unmeasured.

## 1. Verdict

Move three pieces of host work off the unicorn thread. Leave the per-instruction chain where it is.

- **Windowed Linux.** The measured ~1.0 s leaves the critical path. The emulator still copies the frame (section 5), about 1% at ≤60 copies/s, so ~5.6 s becomes ~4.7 s: about 1.2× throughput, about 16% less wall. This needs a spare core. llvmpipe already spawns threads (`user` > `wall` in those runs), so a loaded machine gives back less.
- **Headless.** Display work is absent. The only overlap is the GPU worker. Unmeasured; a few percent on proj7 (a fill plus a few shapes per 865800-cycle frame). A guest that streams full-frame fills gains more. It does not clear 15% on proj7.
- **macOS.** The same split takes Cocoa `PollInputEvents` and the swap off the guest path. Unmeasured. If `EndDrawing` waits for vsync, the stall is larger than the Linux 18%. Measure `end_draw` and `poll` first (`docs/PROFILING-PLAN.md`, run F) before treating the macOS gain as known.

`--sync` (section 8) keeps today's one thread so `make test` stays bit-identical.

## 2. What the FPGA actually overlaps

`ml505top.v` clocks, from the 100 MHz input:

| Domain | Rate | What runs there |
|---|---|---|
| `cpu_clk_g` | 50 MHz | CPU, both caches, `RequestController`, GPU, PixelFeeder fetch, UART, COP0 |
| `dvi_clk_g` | 40 MHz | DVI, read side of `pixel_fifo` |
| MIG `clk0` / `clkdiv0` | 200 / 100 MHz | DDR2 PHY |

`Memory150.v` crosses `cpu_clk_g` to the MIG with `mig_caf`, `mig_wdf`, and `mig_rdf`. `pixel_fifo` is written on `cpu_clk_g` and read on `dvi_clk_g`. Everything else shares the CPU clock.

`RequestController.v` grants one DDR beat at a time, fixed priority: I-cache, D-cache, GPU command fetch, PixelFeeder, FrameFiller, LineEngine, bypass/ScanLineRunner. FrameFiller and LineEngine's write ports are tied off; pixels leave through one ScanLineRunner port, one engine at a time (lowest index: fill, line, ellipse, rect).

A launch is a one-cycle pulse. `MemMapIO.v` raises `gp_vcode` for one cycle. `GraphicsProcessor.v` starts only on `T_START = GP_ready && GP_vcode`, so a `GP_CODE` write while busy is dropped. `GP_ready` is true only in `MS_IDLE`. `irq_gp_done` (`GPU.v`) is the rising edge of ready. The CPU keeps issuing; it stalls only on a cache miss (`Cache.v`, `stall_top` in `ml505top.v`).

vvizard today runs the list inside `mmio_write` (`src/main.c`, `VV_OW_GP_GCODE`) via `vv_gpu_run`, and `vv_gp_launched` (`src/hw.c`) sets every engine ready and raises `VV_IP_GP` before the store returns.

## 3. Splits that lose

| Split | Why it loses |
|---|---|
| Thread per clock domain | The 50 MHz domain is one FSM mux plus two FIFOs. A thread per module spends the ~93 ns instruction budget on wakeups. |
| DDR arbiter thread, one queue slot per cached access | Needs a mem hook on the DDR mapping (slow store path, `docs/PROFILING-PLAN.md` culprit 2) or `uc_*` from a second thread. Unicorn is not thread-safe for concurrent `uc_*` on one engine. Adds miss stalls the model does not have. |
| One thread per GPU engine | ScanLineRunner already serializes writes. A fill is memory-bound. |
| PixelFeeder fetch thread | Guest-visible PF behavior is the period compare in `vv_hw_tick` (`VV_PF_PERIOD` = 865800). A 40 MHz fetch loop rereads a buffer the window copies at 60 Hz. |

UART baud and the COP0 timer stay counters inside `vv_hw_tick`. So does stdin `poll` (at most every 4096 cycles, `vv_host_rx_avail`). A helper thread that reads stdin early would move the RX start cycle; the emulator keeps the poll.

What stays on the emulator every instruction, threaded or not:

- `insn_count`, `vv_hw_tick` (UART RX/TX, timer, RTC, PF boundary, GPU ready edge)
- `vv_quirk_post` / `irq_step` / `vv_quirk_pre`, including `fetch`'s 4-byte `uc_mem_read`
- MMIO callbacks, which run inside the store, before the next `hook_code`

Unicorn's own per-instruction helper call is the rest of the headless time. Threading does not remove it.

## 4. Three threads

1. **Main.** `InitWindow`, `PollInputEvents`, byte swizzle, `UpdateTexture`, strip, `EndDrawing`. No `uc_*`. No emulator globals (`insn_count`, quirk state, `vv_hw_*`).
2. **Emulator.** `uc_emu_start` and `hook_code`: `vv_hw_tick`, `vv_quirk_post`, `irq_step`, `vv_quirk_pre`, MMIO, UART, the PF frame counter. Owns the engine.
3. **GPU worker.** One command slot. Writes pixels into the DDR host buffer. No `uc_*`, no `vv_eprintf`. Returns a result struct (rc, command count, fault). The emulator prints the `[gpu]` line and folds counts into the `[hw]` summary.

`hook_code` stays on the emulator, in today's order: `vv_hw_tick`, then `vv_quirk_post`, then `irq_step`, then `vv_quirk_pre`. Quirks and `irq_step` patch registers around each instruction; they do not move. Raising `VV_IP_GP` inside `vv_hw_tick` makes that edge visible to `irq_step` on the same instruction, which is how UART, PF, timer, and RTC already work.

Startup stays single-threaded through `vv_disp_init`, image load, and `--random-ddr`. With a window, main then creates the emulator thread and the GPU worker and loops on events. With `--no-window`, main creates the emulator and the GPU worker and joins them; it does not call raylib (`vv_disp_init(0)` already skips `InitWindow`). Every 4096 instructions the emulator samples three atomics: quit, `want_frame`, and the GPU done flag (done is also sampled from `vv_hw_tick`, so the ready edge is not delayed until the next 4096 boundary).

## 5. Handoff

**Frame.** DDR becomes a host buffer (`uc_mem_map_ptr`, section 7). `vv_disp_pf_write` stops calling `draw` on the emulator. It records `cur_frame` and requests a copy. The emulator `memcpy`s the displayed frame into a free slot of a triple buffer at two times: a PixelFeeder switch (from `vv_hw_tick`, about 12/s at 10.7M instructions/s with the default period), and when main sets atomic `want_frame` (main aims at 60 Hz; the emulator notices on the 4096-instruction check). The copy is the visible rows only, 600 × 3200 B, raw big-endian, strided by `VV_ROW_PITCH` (4096). About 0.1–0.3 ms. A switch and a pending `want_frame` share one copy, so the emulator pays at most ~60 copies/s, about 1%.

Slots are three raw buffers. Main publishes the index it is swizzling; the emulator writes any other slot. If main is still on the previous slot and the third is the latest publish, the emulator overwrites the unpublished one. It does not wait. Publish is a release store of the slot index and a generation. Main acquire-loads that index, swizzles (the byte loop in `grab`: big-endian XRGB to RGBA), `UpdateTexture`s, draws the strip from the snapshot below, and `EndDrawing`. Main never reads guest memory.

Reading the mapped buffer from main skips the memcpy and tears where the CPU or the GPU worker stores mid-copy. The picture is still fine. The concurrent read is a formal data race. The triple buffer is the default.

**Strip.** `draw_strip` calls `vv_status_lines(uc, …)` (`src/main.c` ~430), which `uc_mem_read`s proj7 shared memory through `rd_guest` and reads `insn_count`, `pf_writes`, `gp_launches`, `irq_count`. It also calls `vv_lcd_get` for 32 chars. The main thread draws from a snapshot. The emulator formats the two lines at most `STATUS_HZ` (4 Hz) or when `vv_disp_status_dirty` is set, and copies the 32 LCD bytes into the same publish. `draw_strip` on the main thread uses those bytes and does not call `vv_status_lines` or `vv_lcd_get`.

**Keys.** An SPSC ring of 256 bytes replaces `kq` (`src/display.c`). The main thread is the only producer (window keys from `collect_keys`); the emulator is the only consumer, from `vv_host_rx_avail` / `vv_disp_key_take`, with the same priority as today: `--uart-in`, then the script queue, then this ring, then stdin. A full ring drops, as `kq_put` does. The ticker is drawn only on main. Window keys are recorded when main enqueues them. Bytes the emulator accepts from `--uart-in`, the script, and stdin are copied into the snapshot (a short side log, same 64-key cap as `kl[]`); main paints them. The emulator does not write `kl[]`. The cycle a window key enters the shifter is already wall-clock dependent: today `PollInputEvents` runs every 4096 instructions, against whenever the OS queued the key.

**Commands.** The worker reads command words from the host DDR buffer while it runs. That matches the RTL: `GraphicsProcessor` fetches through `raf_*` during `MS_READ` / `MS_PROC`, and it does not snapshot the list. A launch-time copy is the wrong default: `vv_gpu_run` allows up to `MAX_WORDS` (1<<20) words before the runaway stop. The CPU can store into the list during the draw; the FPGA allows the same race. `--sync` keeps today's `uc_mem_read` walk inside `vv_gpu_run`.

## 6. Guest-visible GPU behavior

On `GP_CODE`, if the worker is busy, drop the launch (`T_START`). Otherwise, still inside `mmio_write` on the emulator thread: snapshot D-cache lines (below), clear every engine ready bit in `vv_gp_status_word`, set the completion cycle to "not yet known", and release the worker with the code address, the frame, and the launch `insn_count`. Ready is already 0 before `mmio_write` returns, so the next instruction's `GP_STATE` read observes busy even if the worker has not been scheduled.

The worker walks the list in the host buffer (a counting pass, then the drawing pass) and counts pixels it will write, the same pixels `span` / `slr_run` write today (`src/gpu.c`). It release-stores `completion = launch + pixels/4 + 32 * commands` before the stores finish. One 128-bit beat is four pixels. A full 800×600 fill is 120,000 cycles. A line is `max(|dx|,|dy|)+1` pixels; a rect or ellipse is the pixels those engines actually store, not the bbox area. `vv_hw_tick` sets the ready bits and raises `VV_IP_GP` on the rising edge when `insn_count` has reached that cycle and the worker's done flag is set. The worker usually finishes the stores first; `GP_WAIT()` still spins until the cycle. If the worker is late, ready waits for both. A fault (bad opcode, or no `STOP` within `MAX_WORDS`, the -2 / -1 returns from `vv_gpu_run`) sets done with `gp_fault` and takes the same edge.

**`gp-busy`.** A dropped launch is a guest bug, same class as `tx-busy` / `rx-overrun`. `software/common/graphics.h` defines `GP_READY()` / `GP_WAIT()` (`GP_STATE.f.gp_ready`); software that writes `GP_CODE` without waiting loses the list on the FPGA. Warn by default via `vv_quirk_bugs()`, silence with `--quiet-quirk=gp-busy`, halt with `--halt-on-quirk`. Count drops in the `[hw]` summary next to the existing UART lost/overrun counters.

**D-cache.** Before the worker is released, the emulator runs a `vv_dc_dma_write`-style pass (`src/dcache.c`) over valid lines that overlap the target frame. At most 256 lines, misses return immediately. The detector (`src/quirk.c` ~547) reports only when the cached word differs from current DDR, so marking a line the GPU never writes does not false-positive. Gap: a line filled while the worker is still drawing can miss staleness (false negative). `--sync` (inline `vv_gpu_run`) keeps today's exact behavior.

**Translations.** `uc_mem_write` drops Unicorn translation blocks; a store into the `uc_mem_map_ptr` buffer does not. Guest code does not normally live in a framebuffer (proj7 runs from I-scratch, BIOS, ISR BRAM). If a GPU target range overlaps DDR pages that have been executed, the emulator calls `uc_ctl_remove_cache` on those pages at completion, on the emulator thread. If that range is awkward to name, that launch takes the `--sync` path.

## 7. DDR, logging, shutdown

**Mapping.** The 256 MiB DDR region is `uc_mem_map_ptr` over one host buffer, allocated by the host and passed in (Unicorn does not own it). Aliases `0x2` / `0x3` still resolve through `tlb_fill` / `vv_canon` onto that buffer. `--random-ddr` (`fill_ddr_random`) and the loaders keep using `uc_mem_write` on the emulator thread, before `uc_emu_start` and before any worker exists. BIOS, D-scratch, I-scratch, MMIO, ISR BRAM, and the VOID page stay `uc_mem_map` and are touched only from the emulator thread.

**Synchronization.** C11 atomics (`stdatomic.h`, fine under `-std=gnu99`). No mutex on the instruction path.

- `quit`, `want_frame`: main release-stores, emulator acquire-loads on the 4096 check.
- Frame slot index and generation: emulator release-stores, main acquire-loads. The snapshot beside it (status lines, 32 LCD bytes, emulator key-log) is written before that store and read only after the acquire.
- GPU slot: one struct (`code`, `frame`, `launch_cycle`, `completion`, `rc`, `cmds`, `fault`, `done`). Emulator fills it, release-stores `busy`, and posts a semaphore. The worker waits on that semaphore (launches are rare; it must not spin). It release-stores `completion`, then the pixels, then `done`. `vv_hw_tick` acquire-loads `completion` and `done`.
- Key ring: `head` written by main, `tail` by the emulator, both with release on the index store. Capacity 256, drop on full.
- `vv_eprintf` mutex, as below. Never taken by `hook_code` except when a quirk or `[gpu]` line is actually printed.

**Logging.** `vv_eprintf` keeps `err_bol` (`src/term.c`) and is not thread-safe. Take one mutex around `vv_eprintf` / `vv_veprintf` for the whole line. `raylib_log` (`src/display.c`) runs on the main thread and takes the same mutex. The GPU worker never prints. UART stdout (`vv_uart_out`) stays on the emulator thread and does not take that mutex.

**Quit and teardown.** The main thread sets an atomic quit flag when the window closes. `hook_code` polls it on the same 4096-instruction check as `vv_disp_tick` and calls `uc_emu_stop`. The emulator publishes one last frame slot before it returns. After join, one thread touches the engine: dump-frame reads the DDR host buffer (guest memory, not the snapshot, so the PPM matches `--sync`), `--expect` and the register dump use `uc_*`, and `vv_hw_summary` / `vv_quirk_summary` run. `--screenshot` and `--hold` then run on main against that last slot. `--screenshot` still requires a window. `--hold` polls events on main until close, and does not resume the guest.

## 8. Switches

| Switch | Behavior |
|---|---|
| `--sync` or `--threads=1` | No `pthread_create`. Inline `vv_gpu_run`, ready bits always set, `VV_IP_GP` at launch, `grab` via `uc_mem_read`. Same path as today. |
| default, window on | Main + emulator + GPU worker. |
| default, `--no-window` | Async GPU still on (the ready bit and `gp-busy` exist headless). No display threads. |
| `make test` | Passes `--sync`. |

No `THREADS=1` build switch. Add `-pthread` to `CFLAGS` and to the link line. `FLAGS_LINE` already rebuilds the tree when those flags change. `make TSAN=1` appends `-fsanitize=thread` to `CFLAGS` and the link (Linux). `--sync` still links pthreads; it just does not start any.

## 9. macOS

Raylib/GLFW stays on the main thread (`InitWindow` through `CloseWindow`). The emulator and the GPU worker are pthreads. `vv_disp_init` runs on main before the emulator starts. The quit flag, the key ring, and the triple buffer are the only cross-thread inputs. Measure before expecting a number: `docs/PROFILING-PLAN.md` run F, plus `end_draw` and `poll`. An arm64 binary is required (that plan's Rosetta check); ThreadSanitizer on macOS is optional, the Linux `TSAN=1` run is the one that gates the change.

## 10. Testing

- `make test` under `--sync` stays bit-identical to today, including `gp_status` (ready bits and Cause bit 12 in the launch instruction) and `dcache_stale` (fill visible to DDR immediately).
- Run the suite again without `--sync`. Expected to change: `tests/hw/gpstat` (ready low until the busy time, `VV_IP_GP` on the rising edge) and `tests/hw/dcache` (stale snapshot at launch; a reload that races the worker is the known false negative). Add threaded variants that wait until ready, then check the pixels, the ready bits, bit 12, and a `gp-busy` warning on a second `GP_CODE` write while busy.
- `--trace` sync against threaded, no GPU stores: identical. With GPU use: differences only after a launch, in `GP_STATE` reads and `IRQ_GP` timing.
- `make TSAN=1` on Linux (`-fsanitize=thread` on compile and link) over the threaded suite and a short windowed run where the guest `GP_WAIT`s before it reads the frame. Then a windowed run on macOS (Apple Silicon, arm64). TSan does not instrument Unicorn's JIT code, so it cannot see guest stores racing the worker (an accepted race anyway, section 13); it does check the worker, the rings, the snapshot, `err_bol`, and any `uc_*` call from the main thread, and a report on those is a failure.

## 11. Files, when this is implemented

No other behavior change rides along. The edits are confined to:

- `Makefile`: `-pthread`; `TSAN=1`
- `src/main.c`: thread start/join, `uc_mem_map_ptr` for DDR, quit / `want_frame` in the 4096 check, `vv_status_lines` called only from the emulator
- `src/display.c`: triple buffer, swizzle on main, snapshot strip, SPSC ring, `vv_disp_pf_write` no longer calls `draw`
- `src/hw.c`: ready bits, completion edge, dropped-launch counter in `vv_hw_summary`
- `src/gpu.c`: worker entry; pixel stores go to the host buffer; no printing
- `src/dcache.c`: snapshot of valid lines overlapping the target frame, called from the emulator at launch
- `src/quirk.c` / `src/quirk.h`: `gp-busy` in `vv_quirk_bugs`
- `src/term.c`: mutex around `vv_eprintf`
- `tests/test_hw.py` and a threaded variant of `gpstat` / `dcache`: `--sync` on the existing suite, new cases without it

## 12. Implementation order

1. `--sync` / `--threads=1` select today's path and do not create threads. Point `make test` at `--sync`. Confirm the suite is unchanged, including trace output.
2. `uc_mem_map_ptr` for the DDR region only, still one thread, `--sync`. Confirm `--random-ddr`, `--load`, alias accesses (`tests/alias`), and `make test`.
3. Split main / emulator (`src/main.c` around `uc_emu_start`, `src/display.c`). Triple buffer, status and LCD snapshot, key ring, quit flag, `vv_eprintf` mutex. `vv_disp_pf_write` only requests a copy. GPU stays inline on the emulator. A windowed 50M-instruction proj7 run should fall from ~5.6 s to ~4.7 s.
4. GPU worker (`src/gpu.c` drawing into the host buffer, `src/hw.c` ready edge, `src/dcache.c` snapshot, `gp-busy` in `src/quirk.c`). `uc_ctl_remove_cache` when a target overlaps executed DDR. Add the threaded `gpstat` / `dcache` / `gp-busy` variants.
5. `make TSAN=1` on Linux, then the same windowed guest on macOS. Compare `--trace` sync against threaded (section 10).

## 13. Risks

- The 16% windowed figure assumes the memcpy stays ~0.1–0.3 ms and the main thread keeps up. A swap that blocks is the main thread's problem only while the emulator does not wait on a full triple buffer: drop to the latest slot, do not stall `hook_code`.
- Concurrent CPU stores and worker stores to the same DDR words are a data race on the host buffer, the same one the FPGA has when software skips `GP_WAIT()`. ThreadSanitizer will not report it (JIT stores are not instrumented). Do not execute from those pages without `uc_ctl_remove_cache`.
- The completion cycle is unknown for the few microseconds until the worker publishes it. Ready is already 0, so guests only see a slightly longer busy stretch when the worker is descheduled.
- `gp-busy` will fire on guests that today "work" only because ready is stuck at 1. That is the bug the detector is for. proj7 calls `GP_WAIT()` before each launch and before reusing the frame (`software/proj_app7/proj7.c`, and `common/graphics.c` before building a list), and it already treats the launch as asynchronous with the CPU, so its behaviour should stay correct.
- A false-negative `dcache-stale` during an in-flight fill is accepted. `--sync` covers the existing test.
- The mutex around `vv_eprintf` must not be held across `uc_emu_start` or `uc_mem_read`.
