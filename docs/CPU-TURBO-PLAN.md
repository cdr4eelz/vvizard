# `--cpu=turbo` plan

Branch: `e/cpu-turbo` (from `main` at `932a0e3`).
Goal: make the guest as fast as we can without a JIT (no runtime code generation).
Speed is the only design goal. Correctness means “runs the current MIPS150 ISA and
cs150 devices well enough that GIOS + proj7 and the usual player/SM programs work,”
not bit-identical traces with `--cpu=fast` or `--sync`.

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

## Speed tricks (all short of a JIT)

No `mprotect`, no emitting host instructions, no DynASM/libjit/LLVM.

Interpreter:

- Computed-goto (or a thread-next “direct threaded” IR: each IR node stores
  the address of its handler).
- Decode-once BB cache (start from today’s 4096-slot idea; size after
  measuring miss rate on proj7). Invalidate only when a store hits a page
  that holds cached code (same code-page filter idea, but keyed on turbo’s
  own host pointers).
- Block chaining / predicted successor; no hash on the hot fall-through.
- Superblocks later if the profiler says hash + event checks still dominate:
  fuse a short run of blocks until a backward branch or MMIO.
- Delay slot folded into the branch op (one IR, not a separate insn + flag).
- Load-delay as a single `rt` + `old` pair, like today’s `vv_fast_cpu`, not a
  pipeline walk.
- `gpr[0]` never written; handlers that write `rd`/`rt` skip when the target
  is 0.
- Specialize RAM loads/stores in the IR when the addressing nibble is known
  at decode (PC-relative, or a recent `lui`+`addiu` to a constant region).
  Everything else stays a fast nibble table.
- Big-endian word load: one `bswap32` of a 32-bit load on LE hosts, not four
  shifts, on aligned `lw`/`sw`.
- No `OP_SLOW` trampoline and no “maybe emulate MIPS32” fallback. An opcode
  the FPGA treats as nop/zero/`addr 0` is a dedicated cheap handler.

Events and devices:

- Next-event countdown instead of per-insn `run_due_events`.
- Increment Count in bulk at the end of a block (`Count += n`).
- PF is “insn_count + period”, not a divide.
- UART TX busy is a single `tx_ready_at` compare at event checks.
- GPU completion is an atomic flag, not a mutex on the CPU path. Mutex/cond
  only if the worker has to sleep when idle (or a park/unpark).
- Do not pre-walk the command list to compute busy cycles. Turbo does not
  need `--event-trace` identity with `--sync`.

Host:

- Keep default LTO (`-flto=auto`).
- `-fomit-frame-pointer`, `-fno-exceptions` (already C), consider
  `-fno-plt` / hidden visibility on turbo internals so calls stay direct.
- `always_inline` on the RAM load/store helpers; `noinline` on cold MMIO
  (LCD, UART, PRNG).
- `likely`/`unlikely` on nibble == RAM vs MMIO.
- One compilation unit for the turbo CPU hot files so the compiler can see
  every handler (or LTO makes that equivalent).
- Optionally `madvise(MADV_HUGEPAGE)` on the 256 MiB DDR mapping; measure,
  do not assume.
- No tracing, quirk decode, or debug dumps compiled into the hot path
  (`#ifdef` or a separate `turbo_trace` file that turbo does not call).

Explicitly out of scope: any JIT, tracing JIT, copy-and-patch that writes
executable pages, or calling Unicorn as a fallback for “unsupported”
opcodes.

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
