# VVIZARD design notes

Background and probe findings moved out of README.md. Nothing here is needed to build or run vvizard.

## Memory map: hardware references
- The hardware decodes only `ADDR[31:28]` (MemBank.v:51,70; StageMW.v:6-21).
- DDR: 0x1 = D$ path, 0x2 = I$ path, 0x3 = both; address `{4'h0,addr[27:0]}` (MemBank.v:141,150).
- BIOS: indexed by `addr[13:2]` (MemBank.v:160); data-read-only in hardware.
- MMIO: MemMapIO.v; registers repeat every 0x100 (`addra[5:0]`, MemMapIO.v:67).
- ISR BRAM: PC_ISR=0xC0000180 (CPUMIPS.v:6).
- PF_FRAME reads return `{4'h1, frame#, 22'b0}` (MemMapIO.v:152).

## kseg / address translation
By default Unicorn's MIPS model uses the CPU TLB with the standard segments:
kseg0/kseg1 (0x80000000–0xBFFFFFFF) map to physical addresses with the top bits stripped, and kseg2/3 (0xC0000000 and up) go through the TLB.
I tested this with a probe program:
- With the default `UC_TLB_CPU` mode, `uc_mem_map(0x80000000)` and a fetch at 0x80000000 fails with FETCH_UNMAPPED.
- Mapping physical page 0 and running at 0x80000000 *does* execute it, which shows kseg0 is translated.
- 0xC0000000 and 0xE0000000 raise a TLB exception.
- kuseg (below 0x80000000) is identity-mapped only while reset Status (0x20400004: ERL=1, BEV=1) is in effect. Once code writes Status and clears ERL, kuseg would also need TLB entries.

**Approach:** `uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL)` plus a `UC_HOOK_TLB_FILL` hook.
The hook maps each virtual address to `base | (addr & (size-1))`, page by page. This bypasses kseg handling and the CP0 TLB, so a write to Status no longer affects address translation.
It also reproduces the hardware's top-nibble aliasing: 0x3xxxxxxx reaches the same DDR as 0x1xxxxxxx, and the BRAMs wrap every 16 KiB.
MMIO uses `uc_mmio_map`. Its callbacks fold the offset with `& 0xFC` to reproduce the 0x100 alias.
The loader writes through `vv_canon()`, so you can also load at alias addresses.


## Unicorn CP0 vs. the COP0150 hardware 
These Unicorn 2.1.1 behaviors were confirmed with a probe program:
- Count stays at 0 (there is no running timer).
- A write to Cause keeps only IP1:0 (bits 9:8).
- EPC is writable.
- Neither a Count==Compare match nor an Unicorn-raised interrupt happened.
- There is no `UC_MIPS_REG_CP0_CAUSE`, EPC, Count or Compare register ID, only STATUS/CONFIG3/USERLOCAL. Reading or writing those registers from the host requires executing mfc0/mtc0 stubs or using hooks.


## PixelFeeder details
- A PF_FRAME write (0x80000050) selects the frame using `FRAME_BITS` (`gpcommands.vh:39`): a frame number, or an address with the frame number in bits 27:22. The new frame takes effect immediately; real hardware waits for the end of the current frame scan.
- The window is redrawn on every PF_FRAME write and about every 1/60 s of host time, checked every 4096 instructions.
- Pixels are read in plain address order. Real hardware may mirror each group of 4 pixels (`PixelFeeder.v:10,127`); the user chose not to emulate that.

## GPU details
- The interior fill rule is from ScanLineRunner.v:127.
- ELIP is a port of ElipseEngine.v (midpoint, 32-bit math, one ScanLineRunner run per row per step). Its spans wrap at 10 bits (no clipping), so off-screen rows/cols land in frame DRAM as on hardware. LINE/RECT/FILL are clipped to 800x600.
- The XVAR/YVAR fields of point words are ignored.

## Test pass conditions
Hex programs in `tests/*.hex`; convert with `perl -ne 'print pack("H*",join("",split))'`.
- `loop` (at 0x40000000): passes if t0=30 and t2=0x11e.
- `uart`: writes "Hi\n" to 0x80000008, to the alias 0x80000108, and as a byte store to 0x8000000B. Passes if stdout is exactly `Hi\n` and t2 (the TX_READY read) is 1.
- `alias` (runs at 0xC0000180): stores to 0x12003000 and reads back through 0x32003000, before and after `mtc0 $0,Status`.
- `frame`: fills x 100..131, y 50..59 of frame 1 with 0x00FF8000, writes 1 to PF_FRAME, then `break`.
- `prng` (source `tests/prng.s`, at 0x40000000, `--max-insns 300000 --no-random-ddr`): reads the PRNG unseeded, stores 0xDEADBEEF and reads at +1 (stale), +2, +3, +5 cycles, reseeds with 0, `sb` at +3 and at +0, `lbu`, a store through the 0x80000180 alias, a CNT_RESET store (must not disturb the PRNG), then reads after a 196608-instruction loop (exercises the jump-ahead). Passes with
  `--expect a0=0x56709234 --expect a1=0x2b38491a --expect s0=0xe28b8493 --expect s1=0xdeadbeef --expect s2=0xef76df74 --expect s3=0x3bddb7dd --expect s4=0x1 --expect s5=0xab --expect s6=0xab000000 --expect s7=0x0 --expect t3=0xdeadbeef --expect t5=0x46dd8d7a`.
  These values were checked against the RTL: an Icarus testbench drove cpu/MemMapIO.v with the same MMIO access in the same cycle (cycle = instruction number, state(0) = reset value) and read the same 12 values.
- `gpcmds` + `gplaunch`: load gpcmds at 0x12003000 and gplaunch at 0x40000000, run with `--dump-frame`, check with `tests/check_gp.py`.
- `test-player` (`make test-player`, ~1.5 s): boots GIOS -> proj7 headless with `--no-random-ddr` and an
  `--input-script`. proj7 reads player input only in its UART-RX ISR (isr7.s `ISR_UARX`): keys
  `u i o / j k l / m , .` = up-left, up, up-right / left, centre (no-op), right / down-left, down, down-right;
  orthogonal = 6 px, diagonal = 4 px per key. Each of 12 segments (idle, up, idle, down, left, right, up-left,
  idle, up-right, down-left, down-right, `k`) sends the key 3 times, one byte every 2 PF_FRAME writes, then dumps
  the displayed frame 6 frames later. The player ellipse (colours 0xFF6644/0x4466FF) is located by bbox centre.
  Passes if every segment's displacement equals 3x the per-key delta within 1 px (idle/`k` => 0,0) and the start
  position is (400,300). Measured: exact (±18 / ±12 px, idle 0).

## Input script and interrupt injection
- `--input-script F`: lines `fN|iN key STR` (queue STR on UART RX at the Nth PF_FRAME write / insn N; escapes
  `\r \n \t \\`, `\s`=space) or `fN|iN dump F.ppm`. Implies `--irq`. RX priority: `--uart-in`, script queue, window keys, stdin.
- `--dump-every N [--dump-prefix P]`: dump the displayed frame on every Nth PF_FRAME write to `P%06d.ppm`.
- `--irq`: COP0150-style UARX interrupt injection. Unicorn cannot set Cause IP bits or expose Cause/EPC, and its own
  exception entry (MIPS vector, EXL) differs from COP0150, so vvizard does it by hand in the code hook: when
  Status bit0 (IE) and bit10 (IM_UARX) are set and RX data is available (and the current insn is not a delay slot),
  it sets shadow EPC=pc, clears Status.IE and jumps to 0xC0000180. `mfc0 $13/$14` inside the ISR BRAM get the shadow
  Cause (bit10 = RX valid, level) / EPC; `mtc0 $13/$14` update the shadow. The ISR's `jr $k0; mtc0 Status` return
  runs natively. Only UARX is injected (no TIMER/RTC/UATX/GPU), so the proj7 clock never ticks.

- Window keys (display.c): every 4096-insn tick calls PollInputEvents (or EndDrawing on redraw) then drains
  GetCharPressed/GetKeyPressed into a 256-byte queue feeding UART RX; SetExitKey(KEY_NULL) so Esc is sent as 0x1b.
  `make run` now passes `--irq` by default; GIOS boot + `jal` autorun unaffected (GIOS polls with IE off; 0 irqs).
  Verified under Xvfb :77 + xdotool: 5x `l` -> 5 irqs, player (400,300)->(430,300) in --dump-frame.

## Hardware-quirk detection (`--halt-on-quirk` / `--warn-on-quirk`, `src/quirk.c`)
- Hooks: no new Unicorn hooks. The existing `UC_HOOK_CODE` calls `vv_quirk_post()` (checks the PREVIOUS
  instruction's result: `lb`/`lh` rt with bit 7/15 set), then `irq_step()`, then `vv_quirk_pre()` (reads and
  decodes the instruction about to run; GPRs are read only for loads/stores, add/addi/sub). If `irq_step()`
  redirected to the ISR, the pending load state is dropped instead (that instruction did not run).
  Instruction fetches the hardware would see as 0 (undecoded nibble or MMIO) are caught in `tlb_fill`
  (ignored after a CPU exception or the max-insns stop, when Unicorn heads for its own vector).
- Halting: `uc_emu_stop()` from the code hook stops before the flagged instruction executes (checked: `jalr t1`
  leaves t1 unchanged). `hook_intr`/`hook_unmapped` stay quiet after a quirk halt. Exit status 4.
- `load-use` uses "next executed instruction", so a load in a branch delay slot is checked against the
  branch target, like the pipeline. Registers read per instruction: see `reads()` in quirk.c.
- Speed (20M insns GIOS+proj7, headless, -O0): off 7.1 s, warn 8.2 s, halt 8.8 s.
- `--warn-on-quirk` dedupes on (category, PC) in a 4096-entry table and keeps Unicorn semantics, so a
  flagged BIOS store still sticks (as without the option) and an unaligned/unmapped access still faults.

### Findings on current GIOS / proj7 / isr7 (Oct 28 2026)
- Headless, `--warn-on-quirk`: 20M insns idle; 60M insns with ~1180 scripted keys (moves, long runs off every
  screen edge, space, d/e, junk keys); 70M insns with ~25 GIOS commands (lw/lbu/lhu/sw/sb/sh/dump/gs/result/
  color/fill/line/rect/elip/pf/sf/back/copy/unknown) then proj7 + keys; `test_player` with `--halt-on-quirk`.
- (lb-sign check since removed: the Verilog will be fixed to sign-extend.) Only trip in program code was: **`lb-sign` in isr7.s `_uarx_player_move`** at 0xC0000404 (`lh SMO_player_x`,
  isr7.s:323) and 0xC0000414 (`lh SMO_player_y`, isr7.s:328), when the player x/y goes negative (moved past the
  left/top edge). Benign for the ISR itself: it only does `addu` + `sh`, and the low 16 bits are identical.
- User-driven only: GIOS `lw 70000000` -> `unmapped` at 0x40000200 (gios.c:71 BC_LOAD); `lw 50000001` ->
  `unaligned` at the same PC; `sw X 40000100` -> `bios-store` at 0x40000284 (gios.c:89 BC_STORE).
- Static scan of the dumps: every `jalr` in gios/proj7 has rd=$31 (0x40001f04 `jalr zero,zero` and the
  mfhi/mthi/mflo/addi hits at 0x40001fdc+ are data words); gios's 12 `lb` read string bytes, so they trip only on
  bytes >= 0x80; no add/sub, mult/div, syscall, lwl/lwr in code.
