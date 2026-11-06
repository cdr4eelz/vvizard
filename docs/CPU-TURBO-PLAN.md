# `--cpu=turbo` plan

Branch: `e/cpu-turbo` (from `main` at `932a0e3`).
Goal: make the guest as fast as we can. Speed is the only design goal.
No JIT (no runtime code generation) and no hit-based tricks (see “Leaving out”).
Correctness means “runs the current MIPS150 ISA and cs150 devices well enough
that GIOS + proj7 and the usual player/SM programs work,” not bit-identical
traces with `--cpu=fast` or `--sync`.

## Why not extend `--cpu=fast`

`src/cpu/fast.c` is already a predecoded basic-block interpreter, but it is wired
into the shared world:

- `vv_fast_env` carries a Unicorn engine and function-pointer MMIO (`mmio_read` /
  `mmio_write` in `main.c`).
- Decode consults `quirk.h` on every instruction (`vv_quirk_on` for overflow, hilo,
  reserved, lwlr, unmapped, …) and can fall through to `OP_SLOW`.
- Loads/stores go through `vv_canon_inl` then either host RAM or the generic MMIO
  callbacks, which also implement DDR-stall / D-cache / quirk reporting.
- The GPU worker in `src/gpu.c` first walks the command list on the CPU thread
  (`vv_gpu_count`) so busy time is known, then may `vv_gpu_wait` at the ready
  edge so traces stay repeatable.
- Windowed mode still snapshots GPRs into Unicorn at publish (`vv_fast_publish_prepare`).

Turbo should not take any of those dependencies. It gets its own CPU, memory map,
MMIO, and GPU in `src/turbo/`, called only when `--cpu=turbo`. `--cpu=fast` and
`--cpu=unicorn` stay as they are.

## Closed world

`--cpu=turbo` is one fixed machine. It rejects (does not implement) every knobs
that exist to trade speed for fidelity or generality:

- No quirk *options*: no `--quirks`, `--no-quirks`, `--halt-on-quirk`,
  `--warn-on-quirk`, `--list-quirks`, `VVIZARD_QUIRKS`, no decode-time
  `vv_quirk_on`. Hardware behavior is compiled in (see ISA below).
- No `--ddr-stalls`, no I$/D$ model, no per-access stall accounting.
  Cycles equal instructions (1:1). `C` and `I` on the status strip will match,
  so `I` shows as `-` with the existing display rule.
- No `--sync` / `--threads=` for this backend: the GPU always runs on its own
  thread. (The raylib window still has to live on the main thread.)
- No Unicorn, no TLB hook, no `uc_mem_*` on the hot path.
- No little-endian guest, no FPU, no MIPS32 features the FPGA does not have.
- Host conveniences that do not sit on the guest hot path may remain:
  loading the four memory images, window / headless, `--max-insns`, `--max-time`,
  UART, `--dump-sm`, screenshot, the C/I/F strip. Anything that needs a
  turbo-incompatible flag is an error, not a silent downgrade.

## ISA: current MIPS150, baked in

“No quirks” means no quirk *framework*. The instruction set *is* the current
cs150 CPU (the default column in `src/quirk.h`, hardware commit `fcdda7f`),
hardcoded:

- Integer R/I/J that the FPGA decodes: `addu/addiu/subu`, logicals, shifts,
  `slt*`, `lui`, `lw/lh/lhu/lb/lbu/sw/sh/sb`, `beq/bne/blez/bgtz`,
  `bltz/bgez/bltzal/bgezal`, `j/jal/jr/jalr`, `mfc0/mtc0`, `break`.
- `add/addi/sub` wrap; no Ov trap.
- No HI/LO: `mult/multu/div/divu/mthi/mtlo` are nops; `mfhi/mflo` write 0.
- `syscall` is a nop.
- Load-use: the instruction after a load sees the old `rt`; if it also writes
  `rt`, its result wins.
- Untaken `bltzal/bgezal` do not link.
- Undecoded nibble load = 0, store dropped; fetch from hole or MMIO = nop.
- BIOS (nibble 4) data stores dropped.
- Store to `0x2xxxxxxx` from non-BIOS code dropped (I-cache path).
- Unaligned `lw/lh/lhu/sw/sh`: load writes 0, store dropped; no AdEL/AdES.
- `lwl/lwr` write 0; `swl/swr` store nothing.
- Unknown SPECIAL funct writes 0 to `rd`. Branch-likely, SPECIAL2/3, COP1–3,
  LL/SC/LWC/SWC/PREF, and COP0 other than `mfc0/mtc0` (including `eret`) are nops.
- `rotr/rotrv` act as `srl/srlv`. Other REGIMM `rt` values follow `rt[0]`/`rt[4]`
  as `bltz/bgez(al)`.
- Opcodes `0x27/0x2C/0x2D/0x2F` access address 0.
- COP0150: Count (9, +1 per insn), Compare (11, reset `0xFFFF`), Status (12),
  Cause (13), EPC (14). `mtc0` to EPC ignored; other registers read 0; `sel` ignored.
- `sb/sh` to MMIO deliver the ByteAccess4 lane-placed word (UART TX and LCD take
  `dina[7:0]`, so `sb` not at `+3` writes 0).
- `jalr` uses `rd` (the old `jalr-rd` bug is *not* present). `lb/lh` sign-extend
  (the old `lb-sign` bug is *not* present).

Device timing that is part of the machine, not a switch: UART TX busy
(4773 cycles), RX one-byte overwrite, GPU busy (a `GP_CODE` store while the
worker is busy is dropped), PF every `VV_PF_PERIOD` (828960) instructions,
COP0 interrupt rules (Cause IP or-ed, Status.IE & IM). No guest-bug *detectors*
and no halt-on-quirk; a dropped TX or GP kick is just what the hardware does.

D-cache is **not** modelled. GPU writes are immediately visible to CPU loads.
That matches “no stalls / no cache tax” and is a deliberate speed vs FPGA
divergence (`dcache-stale` goes away). If a title ever depended on stale
cache lines, turbo will not reproduce it.

## Architecture

Three threads, no Unicorn:

```
  main (raylib)          turbo CPU                    turbo GPU
  present + input   <--triple buffer--           --SPSC job--
                         |    ^                         |
                         |    | shared guest RAM        |
                         +----+---- host pointers ------+
                         inline MMIO
                         (kick = enqueue GPU job)
```

1. **Memory.** Own the nibble map: one 256 MiB DDR buffer (nibbles 1/2/3 alias)
   and four 16 KiB BRAMs (BIOS `0x4`, D-scratch `0x5`, I-scratch `0x6`, ISR `0xC`).
   Guest bytes stay big-endian. Fetch/load/store are pointer math + `bswap` /
   byte extracts. No `vv_nib` Unicorn host mapping required for turbo; the
   loader writes the four images (`gios.inst.bin`, `bench.inst.bin`, `isr7.bin`,
   `proj7.inst.bin`) straight into these buffers.

2. **CPU.** A predecoded basic-block interpreter that never calls out through
   function pointers on the RAM path. Decode once into a compact IR; execute
   from a PC-keyed block cache. Dispatch with computed `goto` (GCC/Clang
   labels-as-values), not a `switch` in the inner loop. Block chaining: each
   block’s last branch carries the predicted successor so the common taken/not
   path does not re-hash the cache.

3. **MMIO.** At decode, classify immediate-offset operations whose base is not
   known; at execute, a first-level nibble test. Nibble `8` goes to a small
   direct-mapped register file for the real offsets (`UATX_*`, `UARX_*`,
   `CNT_*`, `PF_FRAME`, `GP_FRAME`, `GP_CODE`, `GP_STATE`, `RANDOM32`,
   `HALTVAL`, LCD `0x100..0x17C`). Hot registers (`CNT_INST`/`CNT_CYCLE`,
   `GP_STATE`, `PF_FRAME`) are inline. `GP_CODE` enqueues a GPU job (frame +
   code pointer already in guest RAM) and marks the GPU busy. No walk of the
   command list on the CPU thread.

4. **GPU thread.** Always on. Lock-free single-slot (or tiny SPSC) queue:
   `{gp_frame, code}`. The worker walks the existing graphics command set
   (`src/gpu.c`’s `walk` logic, copied/adapted) and stores pixels straight into
   the DDR host buffer. It does not talk to the CPU except via atomics: busy /
   done, and the next PF-visible `GP_STATE`. The CPU does **not** wait for the
   worker at the ready edge. Ready and Cause bit 12 rise when the worker
   finishes, or are sampled lazily on the next MMIO/`mfc0`/event check. A kick
   while busy is dropped (hardware `gp-busy`).

5. **Display.** Main thread only presents. The CPU publishes a frame pointer +
   LCD/status snapshot into the existing triple-buffer idea, without copying
   through Unicorn and without snapshotting GPRs into UC. Prefer publishing the
   GPU-completed frame’s host pointer over memcpy of 4 MiB when the slot can
   hold a reference; copy only if the GPU might overwrite that same frame
   before the present. Input (keys → UART RX) goes the other way through a
   small atomic/FIFO.

6. **Events.** Check interrupts and PF/UART/GPU-ready at basic-block boundaries
   and after MMIO, using a “next event at insn N” countdown so idle ALU blocks
   do not poll every instruction. When Status.IE is 0, skip the IRQ test
   entirely. Enter `0xC0000180` with the COP0150 rules (no `eret`; the ISR
   returns by writing Status).

## Speed tricks

Chosen techniques below. Each should move proj7 Mcyc/s; keep only those that
do, once there is a baseline measurement. None of them generate host code.

### Interpreter

- Direct-threaded dispatch: each decoded IR carries the address of its
  handler (GCC/Clang labels-as-values). Why: the host predictor sees a
  computed goto per opcode instead of one giant `switch`, and there is no
  central dispatch loop.
- Decode-once basic-block cache (start from a 4096-slot table; retune after
  measuring proj7 miss rate). Invalidate only when a store touches a page
  that holds cached code, keyed on turbo’s own host pointers. Why: GIOS and
  proj7 spend almost all time in the same loops, so decode should happen
  once per site, not once per execution.
- Block chaining: each block stores a predicted successor pointer. Why: the
  common fall-through or taken target skips the cache hash.
- Superblocks later, if chaining plus event checks still show up: fuse a
  straight-line run until a backward branch or MMIO. Why: fewer chain and
  event-check points in the code that is already straight-line.
- Delay slot folded into the branch/jump handler. Why: one dispatch instead
  of a separate slot instruction plus an `in_delay` flag test.
- Load-delay as a single pending `rt` + old-value pair (as in `vv_fast_cpu`),
  not a pipeline walk. Why: MIPS150 load-use is one extra move, and a
  pipeline model would cost more than the ISA requires.
- Never write `gpr[0]`; a write to `$zero` is ignored. Why: readers need no
  extra masking if `$zero` stays 0.
- Specialize RAM loads/stores in the IR when the region is known at decode
  (PC-relative, or a `lui`+`addiu` to a constant nibble). Other accesses
  keep a 16-entry nibble table. Why: proj7’s hot loads use a stable base, so
  skipping the full map on those ops saves work on every iteration.
- Region-stable fetch while decoding a block (the sequential PC stays in
  BIOS, ISCR, ISR, or DDR). Why: the top nibble does not change between
  words of one block.
- Aligned `lw`/`sw` use one `bswap32` of a 32-bit host load/store on
  little-endian machines. Why: that is one instruction instead of four
  shifts and ORs.
- Dedicated cheap handlers for what the FPGA treats as nop, write-zero, or
  access-address-0. Why: those opcodes must not fall into a generic
  `OP_SLOW` decoder that exists only to support other option combinations.

### Events, MMIO, and GPU

- Next-event countdown: check PF, UART TX-ready, Compare, and IRQ at block
  boundaries and after MMIO, not after every ALU insn. Why: most blocks
  never touch a device, so per-instruction event polling is wasted work.
- Skip the IRQ test entirely while Status.IE is 0. Why: the guest spends
  long stretches with interrupts off; `mfc0`/`mtc0` already have to look at
  Status.
- Add COP0 Count in bulk at the end of a block (`Count += n`). Why: Count
  is not read every instruction, and adding once per block is cheaper than
  incrementing in every handler.
- PF deadline is `insn_count + VV_PF_PERIOD` (828960), not a divide. Why:
  the next-event check is then one compare.
- UART TX busy is a single `tx_ready_at` compare at event checks (and on
  the TX store itself). Why: the 4773-cycle busy window does not need a
  test on unrelated instructions.
- GPU kick enqueues `{gp_frame, code}` and sets an atomic busy flag; the
  CPU does not pre-walk the command list and does not wait for the worker.
  Why: today’s path steals a full list walk (and sometimes a join) from the
  guest thread, which is the opposite of overlap.
- GPU completion is an atomic done/ready flag. A mutex or park is only for
  putting an idle worker to sleep. Why: a lock on every kick or poll would
  serialize the two threads we added to run in parallel.
- Inline the hot MMIO registers (`CNT_*`, `GP_STATE`, `PF_FRAME`); keep
  LCD, UART, and PRNG as cold helpers. Why: counters and GPU-ready are in
  the guest spin loops; LCD and UART are not.

### Host and display

- Default LTO (`-flto=auto`), `-fomit-frame-pointer`, hidden visibility /
  `-fno-plt` on turbo internals. Why: hot helpers should inline or be
  direct calls, not go through the PLT.
- `always_inline` on RAM load/store helpers; `noinline` on cold MMIO.
  Why: the RAM path stays in the interpreter’s I-cache, and the rare LCD
  or UART path stays out of it.
- `likely`/`unlikely` on “this access is RAM, not MMIO.” Why: almost every
  load/store is RAM, so a hinted nibble test mispredicts less.
- One compilation unit for the turbo CPU hot files (LTO is the fallback).
  Why: the compiler can see every handler and the dispatch graph at once.
- `madvise(MADV_HUGEPAGE)` on the 256 MiB DDR buffer, kept only if it
  measures faster. Why: framebuffer and guest data on 4 KiB pages can burn
  DTLB; 2 MiB pages may cut those misses.
- Publish a host pointer to the completed frame plus an LCD/status
  snapshot; do not copy through Unicorn or sync GPRs into a `uc_engine`.
  Why: a 4 MiB memcpy and a register dump are host work that does not
  retire guest instructions.
- No tracing, quirk decode, or debug dumps in the hot path. Why: those
  exist to support options turbo will not have, and they show up on every
  instruction in `--cpu=fast` when enabled.

## Leaving out

- JIT, tracing JIT, copy-and-patch, DynASM, libjit, LLVM, `mprotect` of
  executable pages, or any other runtime host-code generation. Why: the
  caller ruled out a JIT.
- Hit-based tricks, matching the earlier “short of hit” constraint. That
  means no hotness counters that promote a block once it has been hit
  enough times, no inline caches or region predictors that specialize
  after a runtime address hits a remembered nibble, and no BTB or
  return-stack that steers dispatch based on predictor hits. Why: those
  are the usual on-ramp from an interpreter to generated code, and they
  add mis-speculation paths without being a decode cache. The decode-once
  basic-block cache stays: it memoizes IR so the same PC is not decoded
  again, and it does not emit host instructions when a lookup succeeds.
- Unicorn as a fallback for “unsupported” opcodes. Why: turbo’s ISA is
  closed; a fallback would reintroduce the shared world and its cost.
- Extending `src/cpu/fast.c`, the quirk option machinery, `--ddr-stalls`,
  I$/D$ models, or `--sync`. Why: the single goal is speed in a closed
  machine; those knobs exist to trade speed for fidelity or generality.
- Modelling the D-cache (including `dcache-stale`). Why: a cache model is
  a tax on every miss, and without stalls it is complexity with no speed
  win; GPU writes are immediately visible.
- Waiting for the GPU worker so traces match `--sync`. Why: that join
  puts GPU time back on the guest thread.
- Interpreting guest instructions in SIMD or on more than one CPU thread.
  Why: the guest is a scalar single-issue MIPS150; gathering N instruction
  streams or splitting one PC across threads would cost more than it
  retired.
- Bit-identical event traces versus `--cpu=fast`. Why: that requirement
  forces the pre-walk and ready-edge wait this backend exists to drop.

## Integration

- New `--cpu=turbo` in `main.c` next to `unicorn` and `fast`. Default stays
  `fast`.
- If `opt_cpu == TURBO` and the user also passed `--quirks*`, `--ddr-stalls`,
  `--sync`, `--threads=`, `--halt-on-quirk`, `--warn-on-quirk`, or
  `--no-quirks`, exit 2 with a short message: turbo does not take those.
- Makefile: compile `src/turbo/*.c` into the same `vvizard` binary.
- Tests: a small `tests/test_cpu_turbo.py` that
  - runs the existing assembly exact-instruction cases that encode MIPS150
    behavior (not MIPS32-only cases) on `--cpu=turbo`;
  - boots GIOS+proj7 headless for a fixed insn count and checks it halts /
    prints something sane (HALTVAL / UART), not a cycle-accurate compare
    against Unicorn;
  - checks that incompatible flags are rejected.
  Do not require `make test`’s Unicorn/fast suites to run under turbo.
- Measurement (required before any speed claim): proj7 via GIOS, no quirks
  flags (N/A), no DDR stalls, headless and windowed, median of several
  interleaved runs against `--cpu=fast` on the same machine. Report C
  (Mcyc/s, which equals Minsns/s). No windowed speedup claims without that
  measurement.

## Suggested file split

- `src/turbo/turbo.h` — CPU state, memory, public `vv_turbo_run` / publish /
  stop.
- `src/turbo/mem.c` — buffers, nibble decode, aligned/unaligned RAM access,
  image load hooks.
- `src/turbo/cpu.c` — decode, BB cache, computed-goto execute, COP0, IRQ
  entry.
- `src/turbo/mmio.c` — register file, UART, counters, LCD, halt, GPU kick.
- `src/turbo/gpu.c` — worker thread + command walker writing the DDR buffer.
- `src/turbo/disp.c` or thin hooks in `display.c` — publish host frame
  pointer + LCD/status without Unicorn.
- `docs/CPU-TURBO-PLAN.md` — this file.

Reuse knowledge (command encodings, PF period, MMIO offsets, LCD layout)
from `memmap.h`, `hw.h`, `gpu.c`, and `lcd.c` by copying the constants and
the walker, not by calling those objects through `uc_engine *`.

## Implementation order

1. Memory + loader + a trivial fetch/execute of `nop`/`j` in a unit test,
   no devices.
2. Full baked ISA in the computed-goto interpreter, RAM only; run the
   assembly tests that do not need MMIO.
3. Inline MMIO: counters, halt, UART, LCD, COP0 timer/Compare, PF IRQ.
4. GPU worker + `GP_*` registers; run a small `gp_*.s` and then proj7.
5. Display publish + input; windowed smoke test.
6. Block chaining, next-event countdown, RAM-specialized IR, hugepages —
   each with a before/after proj7 measurement. Keep only what wins.

Do not implement this from `FAST2_SPEEDUP_PLAN.md` or change `--cpu=fast`
while doing turbo.

## Success

Turbo is doing its job when:

- `vvizard --cpu=turbo` (default program) runs GIOS + proj7 and produces a
  recognizable framebuffer / LCD / UART without Unicorn.
- Incompatible flags fail fast.
- Headless proj7 Mcyc/s is measured against `--cpu=fast` on the same host
  and is clearly faster (the point of the backend). There is no numeric
  target until that first measurement exists; after it, further tricks are
  ranked by that same bench.

## First implementation status

The initial independent backend is implemented on this branch. See
[`CPU-TURBO-RESULTS.md`](CPU-TURBO-RESULTS.md) for commands, verification,
measurements, and remaining gaps; see `CPU-TURBO-BENCH.json` for headless samples.

The speed gate rejected the experimental constant-propagated RAM-pointer IR
and huge-page advice. Both were removed rather than keeping complexity that
made proj7 slower. Static block chaining, decode-selected nop-slot fusion,
load-delay pairs, bulk Count, MMIO event deadlines and the independent GPU
worker remain. Other delay slots are in the same block but dispatch normally.

Frame publication currently references live atomic guest pixels, without a
copy-on-reuse lifetime protocol. This is race-safe but can tear when the same
frame is rewritten before scanout completes; it is not the guarded-copy
solution described above. Linux is tested; macOS semaphore parking is not.
There is no windowed throughput claim in this first pass.
