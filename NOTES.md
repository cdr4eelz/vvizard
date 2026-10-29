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
