# Turbo first implementation

Implemented locally on `e/cpu-turbo`; no changes to `src/cpu/fast.c`, no push.
The goal remains speed, not event-trace equivalence or additional fidelity modes.

## Run

```sh
make
./vvizard --cpu=turbo --no-window --sw-dir /workspace/software --max-insns 10000000 --dump-sm
./vvizard --cpu=turbo --window --sw-dir /workspace/software
make test-cpu-turbo
python3 tests/bench_cpu_turbo.py --sw-dir /workspace/software --runs 5 --insns 200000000
```

The normal `../../software` executable-relative path is tried first. Turbo also
tries `../software` for a standalone worktree next to the image directory.
Only the four binary images are needed. Turbo does not load ELF or source files.
RAM is always zero-initialized before image loading; there is no random-DDR knob.
`--max-seconds` is the existing wall-time limit. UART stdin/window keys and
`--uart-in` are supported. `--exit-on-halt` is a host stop convenience, not a
change to the guest HaltVal register.

Turbo uses an explicit allowlist of host conveniences. Quirk flags/environment,
DDR stalls, sync/thread controls, endian changes, IRQ/PF timing controls, legacy
I/O, tracing and other unsupported options fail with status 2, in either order.
Fast remains the default.

## Implemented

- Own 256 MiB DDR and four 16 KiB BRAMs, nibble aliases, big-endian aligned word
  loads/stores, subword operations, and raw image loader. No Unicorn engine is
  opened for turbo.
- Fixed MIPS150 decode, computed-goto handlers, 4096-entry PC block cache with
  32 instructions per block, both static successor edges chained. Indirect
  branches only use the decode cache, not a branch/return predictor.
- One pending load register/old-value pair; `$zero` is never written. Branches
  include their delay slot in the decoded block. Common nop slots have
  decode-selected fused branch handlers; other slots dispatch their own opcode.
- Code-page invalidation uses physical RAM page identity across aliases; CPU
  code stores leave the current block immediately. DMA invalidates the same
  code-page generation at its writes.
- Inline hot counter/PF/GPU reads, Count bulk advancement, deadline-based events (also on MMIO), instruction-clock
  PF at 828960, timer/RTC/IRQ latches and entry, UART TX busy/RX holding byte,
  counters, HaltVal, LCD, and the hardware LFSR PRNG.
- Dedicated GPU thread, all existing graphics commands including ellipse and
  faulting STOP. Release/acquire single-slot mailbox with semaphore parking;
  a busy kick is dropped. No CPU list pre-walk and no guest ready-edge wait.
  Joining the worker happens only on host shutdown.
- CPU, DMA and scanout share aligned atomic RAM words. No D-cache model.
- Host frame-pointer plus LCD/status triple-buffer publication, without Unicorn
  register snapshots. Raylib stays on the main thread and the CPU has its own
  thread when a window is open.
- GCC dispatcher-tail optimizations disabled locally on the interpreter so
  direct-threaded opcode tails are not collapsed into one indirect jump site.
  Default release LTO remains enabled.

## Verification

- `make` and `make test-cpu-turbo` pass. The 13 test methods include 16 existing
  assembly programs (including hwprobe's 45 internal checks), load-delay/zero,
  timer IRQ return, Count writes, alias self-modifying code, jump to address 0,
  instruction limits at delay slots, MMIO lanes and mask-zero read side effects,
  GPU fill/fault and CPU visibility, flag rejection, GIOS/proj7 and window smoke.
- Default program prints the GIOS banner and `PROJ-7.0`, with `MAGIC: fedbeef3`;
  shared memory decodes the player at `(400,300)` and active GPU launches.
  Auto-typing `jal 60000000\rq` with `--exit-on-halt` reaches HaltVal 1 and QUIT.
- Xvfb windowed GIOS/proj7 smoke completes and dumps a framebuffer. This is not
  a windowed performance measurement.
- AddressSanitizer + UndefinedBehaviorSanitizer build passes the turbo suite
  (leak detection disabled; no claim about leaks in third-party libraries).
- Existing `test_cpu_default.py`: 6/6; `test_cpu_fast.py`: 26/26, including
  explicit Unicorn comparisons and threaded window tests.

## Measurements

Final medians: **turbo 372.747 Mcyc/s**,
**fast 62.700 Mcyc/s**, or **5.94x** headless throughput.
See `CPU-TURBO-BENCH.json` for all five interleaved samples.
Measured on the same shared Linux x86-64 box (8 vCPUs, generic Intel Xeon),
GCC 14.2.0, release `-O3 -flto=auto`, on November 6, 2026 (PST).

Both backends retire 200 million guest instructions per run using the same
GIOS/bench/isr7/proj7 images and stdin at EOF. Fast additionally uses
`--no-random-ddr --frame-clock=emulated` to match turbo's fixed zero-DDR and
instruction-clock PF. Neither uses stalls. Cycles equal instructions.
The rate interval excludes image loading and final worker shutdown. Fast's
rate comes from its existing `[frame]` summary; turbo prints its own interval.
The async devices deliberately do not produce identical traces.

Only headless throughput is claimed. Runs are not CPU-pinned, and the machine
is shared; small differences between optimization variants are noisy.

Individual 100-million-instruction A/B experiments (five interleaved pairs):

| Experiment | Before median Mcyc/s | After median Mcyc/s | Decision |
| --- | ---: | ---: | --- |
| Preserve opcode-specific dispatch tails | 261.673 | 275.639 | Keep |
| Apply event deadlines on MMIO polls | 279.198 | 357.747 | Keep |
| Decode-selected fused nop delay slots | 353.423 | 361.935 | Keep (small win) |
| Constant-propagated RAM-pointer IR | 272.500 | 246.030 | Discard |
| `MADV_HUGEPAGE` advice | 270.278 | 218.639 | Discard (noisy, no win) |

## Remaining limitations / plan differences

- Scanout references live atomic guest pixels: memory access is race-safe, but
  a later CPU/GPU rewrite of the published frame can tear. There is not yet a
  copy-on-reuse/immutable-frame lifetime protocol. This avoids blocking the
  guest or copying every frame, but is a gap from the plan's guarded-copy idea.
- Arbitrary non-nop delay slots still dispatch separately, although they are
  folded into the same block with no pipeline/in-delay flag. Superblocks are
  deferred. No hotness promotion, runtime region predictors, BTB, or JIT.
- Decode-time RAM-pointer specialization was implemented and tested, then
  removed because the measured proj7 rate regressed. Generic loads still use
  the fixed 16-entry nibble table. Huge-page advice was likewise removed.
- No screenshot/hold/input-script or ELF support on this path. Unsupported
  options are rejected, not silently ignored. These are not speed priorities.
- The GPU park uses POSIX unnamed semaphores; Linux is tested. macOS needs a
  supported park primitive before claiming that platform works.
- Interrupt/event checks are batched at block boundaries or due MMIO accesses;
  this is not cycle-identical to the other backends. No windowed speed claim.
