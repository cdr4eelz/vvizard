# VVIZARD — a minimal Unicorn-based emulator for the CS150 MIPS150 system

This is a fresh project. It does not use any code from `tool/vizard`. It runs the project's
big-endian MIPS32 binaries (`elf32-tradbigmips`) on Unicorn 2.x, using the hardware memory map.

## Dependencies
Debian 13: `sudo apt-get install -y libunicorn-dev` (Unicorn **2.1.1**). This provides
`/usr/include/unicorn/unicorn.h` and the pkg-config module `unicorn`.

## Build (no Makefile yet — still to be decided)
    mkdir -p build
    CF="-std=gnu99 -Wall -Wextra -O0 -g3 -DVV_DEFAULT_BIG_ENDIAN=1 $(pkg-config --cflags unicorn)"
    gcc $CF -c src/main.c   -o build/main.o
    gcc $CF -c src/loader.c -o build/loader.o
    gcc -o vvizard build/main.o build/loader.o $(pkg-config --libs unicorn)
To make little-endian the default, use `-DVV_DEFAULT_BIG_ENDIAN=0`. You can also pick at run time with `--little` or `--big`.

## Usage
    ./vvizard [opts] [isr.elf ...] [prog.elf]      # entry = last ELF's e_entry
      --load FILE@ADDR  raw binary   --entry ADDR   --sp ADDR (0x50004000)
      --max-insns N (1e6)  --until ADDR  --trace[=N]  --quiet-mmio  --expect REG=VAL
A `break` instruction stops the run cleanly. When the run ends or fails, the registers are printed.
`--expect` sets the exit status: 0 if every check passes, 3 if any fails.

## Memory map (`src/memmap.h` is the single table)
The hardware decodes only `ADDR[31:28]` (MemBank.v:51,70; StageMW.v:6-21).
| nibble | device | canonical base | size / aliasing |
|---|---|---|---|
| 0x1,0x2,0x3 | DDR (0x1 D$, 0x2 I$, 0x3 both; `{4'h0,addr[27:0]}` MemBank.v:141,150) | 0x10000000 | 256 MiB (frames at 0x10000000+N<<22, GPTEMP 0x12003000) |
| 0x4 | BIOS (data read-only in HW; writes are logged here) | 0x40000000 | 16 KiB (`addr[13:2]`, MemBank.v:160) |
| 0x5 | D-scratch = SM_BASE_ADDR shared memory (plain RAM) | 0x50000000 | 16 KiB |
| 0x6 | I-scratch | 0x60000000 | 16 KiB |
| 0x8 | MMIO (MemMapIO.v) | 0x80000000 | 4 KiB page; registers repeat every 0x100 (`addra[5:0]`, MemMapIO.v:67) |
| 0xC | ISR BRAM, PC_ISR=0xC0000180 (CPUMIPS.v:6) | 0xC0000000 | 16 KiB |
Any other nibble cannot be decoded. Accessing one raises a TLB-style CPU exception (`UC_ERR_EXCEPTION`).

### kseg / address translation
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

## MMIO model
- **UART:** TX_READY always reads 1. A write to TX_DATA (offset 0x08, any byte lane) sends byte [7:0] to stdout.
- **UART RX:** RX_VALID polls stdin without blocking, and RX_DATA reads one byte.
- **Counters:** CNT_CYCLE and CNT_INST both return the number of instructions executed since the last CNT_RESET. There is no cycle model.
- **GPU (stub):** PF_FRAME, GP_FRAME and GP_CODE writes are recorded, and a GP_CODE write logs a GPU "launch". PF_FRAME reads return `{4'h1, frame#, 22'b0}` (MemMapIO.v:152). GP_STATE reads return 0, meaning idle.
- **Other offsets:** logged as MISS-READ or MISS-WRITE.
- **Interrupts:** none are injected yet (see the CP0 notes).

## Tests
The hex test programs are in `tests/*.hex`. Convert them with `perl -ne 'print pack("H*",join("",split))'`.
- `loop` (at 0x40000000): passes if t0=30 and t2=0x11e.
- `uart`: writes "Hi\n" to 0x80000008, to the alias 0x80000108, and as a byte store to 0x8000000B. Passes if stdout is exactly `Hi\n` and t2 (the TX_READY read) is 1.
- `alias` (runs at 0xC0000180): stores to 0x12003000 and reads back through 0x32003000, before and after `mtc0 $0,Status`.

## Unicorn CP0 vs. the COP0150 hardware (summary; see report)
These Unicorn 2.1.1 behaviors were confirmed with a probe program:
- Count stays at 0 (there is no running timer).
- A write to Cause keeps only IP1:0 (bits 9:8).
- EPC is writable.
- Neither a Count==Compare match nor an Unicorn-raised interrupt happened.
- There is no `UC_MIPS_REG_CP0_CAUSE`, EPC, Count or Compare register ID, only STATUS/CONFIG3/USERLOCAL. Reading or writing those registers from the host requires executing mfc0/mtc0 stubs or using hooks.

## PixelFeeder display (raylib, `src/display.c`)
- A PF_FRAME write (0x80000050) selects the frame using `FRAME_BITS` (`gpcommands.vh:39`): a frame number, or an address with the frame number in bits 27:22. The new frame takes effect immediately; real hardware waits for the end of the current frame scan.
- The window is redrawn on every PF_FRAME write and about every 1/60 s of host time, checked every 4096 instructions.
- Frame N starts at `0x10000000 + N<<22`. The visible area is 800×600, and rows start 4096 bytes apart.
- Each pixel is a 32-bit big-endian word laid out as XRGB (R=23:16, G=15:8, B=7:0).
- Pixels are read in plain address order. Real hardware may mirror each group of 4 pixels (`PixelFeeder.v:10,127`); the user chose not to emulate that.
- Options:
  - `--no-window` runs without a window.
  - `--dump-frame F.ppm` writes the selected frame to F.ppm when the run ends.
  - `--hold` keeps the window open after the run.
  - Closing the window stops emulation.
- Test: `tests/frame.hex` fills x 100..131, y 50..59 of frame 1 with 0x00FF8000, writes 1 to PF_FRAME, and then hits `break`.

## Default BIOS (gios)
proj_app* programs are linked against **gios**: `FIXED_ELF ?= ../gios/gios.elf` in `software/proj_app6/Makefile:5`. With bios150v3 loaded instead, proj6 jumps to address 0 because the two BIOS images put different functions at the same addresses.
- By default vvizard loads `VV_DEFAULT_BIOS` raw at 0x40000000. The Makefile sets it from `BIOS ?= $(abspath ../../software/gios/gios.inst.bin)`. If the macro is not defined, the path `<exe dir>/../../software/gios/gios.inst.bin` is used.
- The default BIOS is skipped when any `--load FILE@0x4xxxxxxx` is given. `--bios FILE` overrides the default, and `--no-bios` disables it.
- If the default file is missing, you get a warning and the run continues.
- `make run BIN=... ARGS=...` passes `--bios $(BIOS)`.

## GPU command interpreter (`src/gpu.c`)
A write to GP_CODE (0x80000058) runs the GPU on the big-endian command list at that address. It draws into frame `FRAME_BITS(GP_FRAME)` and finishes instantly, so GP_STATE always reads as ready. The GPU-done interrupt (IB_GPU=12) is not raised yet.
| op (bits 31:28) | words | meaning |
|---|---|---|
| 0 STOP | 1 | end of the list |
| 1 FILL | 1 | fill all 800×600 pixels with COLOR28 |
| 2 LINE | 3 | P0, P1: Bresenham, both endpoints included |
| 3 ELIP | 4 | P0 = centre, P1 = (a, b) radii, then XRGB32 fill colour |
| 4 RECT | 4 | P0, P1 = corners: edge colour on the border, fill colour inside, then XRGB32 fill colour |
- A point word is X = bits 25:16 and Y = bits 9:0. The XVAR/YVAR fields are ignored.
- The interior is filled only when bits 31:24 of the fill colour are non-zero (ScanLineRunner.v:127).
- LINE/RECT/FILL are clipped to 800×600. ELIP is a port of ElipseEngine.v (midpoint, 32-bit math, one ScanLineRunner run per row per step); its spans wrap at 10 bits (no clipping), so off-screen rows/cols land in frame DRAM as on hardware.
- Test: load `tests/gpcmds.hex` at 0x12003000 and `tests/gplaunch.hex` at 0x40000000, run with `--dump-frame`, then check the result with `tests/check_gp.py`.
