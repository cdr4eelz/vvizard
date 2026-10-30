# VVIZARD (GitHub: cdr4eelz/vvizard) — a minimal Unicorn-based emulator for the CS150 MIPS150 system

This is a fresh project. It does not use any code from `tool/vizard`. It runs the project's
big-endian MIPS32 binaries (`elf32-tradbigmips`) on Unicorn 2.x, using the hardware memory map.

## Dependencies
- **gcc, make, pkg-config.**
- **Unicorn 2.1.1 or newer** (pkg-config module `unicorn`). VVIZARD needs the virtual-TLB API
  (`uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL)`, `uc_tlb_entry`, `UC_HOOK_TLB_FILL`), which first appeared in 2.1.
  - Debian 13: `sudo apt-get install -y libunicorn-dev` (2.1.1).
  - Ubuntu 24.04: `libunicorn-dev` is **2.0.1, too old** (build fails on `uc_tlb_entry`). Build 2.1.1 into `~/.local` instead
    (needs CMake 3.x or newer; the Xilinx-bundled cmake 3.3.2 is too old):

        curl -LO https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.1.tar.gz
        tar xzf 2.1.1.tar.gz && cd unicorn-2.1.1
        cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local -DUNICORN_ARCH=mips
        cmake --build build -j$(nproc) && cmake --install build

    No exports are needed after that: the Makefile searches `$(HOME)/.local/lib/pkgconfig` first and links with an
    rpath to `$(HOME)/.local/lib`, so the binary loads 2.1.1 instead of the system 2.0.1.
- **raylib** (pkg-config module `raylib`). The PixelFeeder display window uses it, and the program links with `-lraylib`.
  It is needed even if you only run with `--no-window`.
  - Debian/Ubuntu: `libraylib-dev` where packaged. Debian 13 may not package it. If it doesn't, build raylib from
    source (https://github.com/raysan5/raylib) or use a third-party package. This machine has `libraylib6-dev` 6.2.0.
  - Check with `pkg-config --modversion unicorn raylib`.

## Build
The `Makefile` finds Unicorn and raylib through pkg-config:

    make            # build ./vvizard (objects in build/)
    make fresh      # clean + rebuild
    make clean
    make run        # boot GIOS and auto-start proj7 (see "make run defaults")

- `BIOS ?=` defaults to `../../software/gios/gios.inst.bin`, so build GIOS first (`make` in `software/gios`), then
  `software/proj_app6`. Its absolute path is compiled in as `VV_DEFAULT_BIOS`.
- Big-endian is the default CPU mode. Pick one at run time with `--little` or `--big`.
- A Unicorn in `~/.local` takes priority automatically (PKG_CONFIG_PATH and rpath are set in the Makefile, and
  `make run` also sets `LD_LIBRARY_PATH`).

## make run defaults
All images are loaded as raw bytes (`--load FILE@ADDR`), not by ELF sections:
| variable | default | loaded at |
|---|---|---|
| `BIOS` | `software/gios/gios.inst.bin` | 0x40000000 (BIOS; the run starts here) |
| `IMEM_APP` | `software/proj_app7/proj7.inst.bin` | 0x60000000 (I-scratch) |
| `DMEM_APP` | `software/draw/draw.inst.bin` | 0x50000000 (D-scratch) |
| `ISR_BIN` | `software/proj_app7/isr7.bin` | 0xC0000000 (ISR BRAM) |
| `AUTORUN` | `jal 60000000\r` | typed at the GIOS prompt via `--uart-in` |

`ARGS` defaults to `--random-ddr --irq --max-insns 0` (no instruction limit, window on, UART-RX interrupts on so proj7 sees keys). Override any of these on the
command line, e.g. `make run ARGS="--no-window --max-insns 200000"`, `make run AUTORUN=` (stay at the GIOS prompt),
or `make run IMEM_APP=../../software/bench/bench.inst.bin`. `BIN=` replaces all the `--load`/`--entry` arguments.
`QUIRKS` defaults to `--halt-on-quirk` (see [Hardware quirks](#hardware-quirks---halt-on-quirk)); `make run QUIRKS=` turns it off,
`make run QUIRKS=--halt-on-quirk=all,-load-use` skips a category, `make run QUIRKS=--warn-on-quirk` logs without stopping.

**Keyboard:** with the window focused, typed keys go to UART RX: printable ASCII as-is, Enter -> `\r`, Backspace -> 0x08,
Tab -> `\t`, Esc -> 0x1b (Esc no longer closes the window; use the close button). Terminal stdin still works too.
proj7: `u i o / j k l / m , .` move, space pauses, `d`/`e` clock. `--no-window-keys` disables window capture.

**Note:** `bench` and `draw` are linked to run at DDR 0x10000000 (`LDSTYLE := inject_cache`). Their raw bytes can be
placed in the scratchpads, but they won't run correctly from there unless relinked. proj7 (0x60000000) and isr7
(0xC0000000) are linked for where they're loaded. Also, `DMEM_APP` overwrites the start of D-scratch, which proj7
uses as its shared memory (the stack is at the top of D-scratch).

## Usage
    ./vvizard [opts] [isr.elf ...] [prog.elf]      # entry = last ELF's e_entry
      --load FILE@ADDR  raw binary or ELF (repeatable)   --entry ADDR   --sp ADDR (0x50004000)
      --max-insns N (1e6, 0=unlimited)  --until ADDR  --trace[=N]  --trace-regs  --quiet-mmio
      --little | --big   --bios FILE   --no-bios   --no-window   --no-window-keys   --dump-frame F.ppm   --hold
      --random-ddr[=SEED]   --no-random-ddr   --uart-in STR   --expect REG=VAL
      --halt-on-quirk[=LIST]   --warn-on-quirk[=LIST]
A `break` instruction stops the run cleanly. When the run ends or fails, the registers are printed.
`--expect` sets the exit status: 0 if every check passes, 3 if any fails.

### Random DDR at startup
DDR (256 MiB at 0x10000000) is filled with random data before anything is loaded, as real DRAM would be at power-up.
It's on by default, and every run prints its seed, e.g. `[ddr] random fill 256 MiB, seed=1792706903`.
Repeat a run with `--random-ddr=SEED`, or start with zeroed DDR using `--no-random-ddr`. Loaded images overwrite the
random data where they land, and frame buffers start out as noise until something draws into them.

### Typing into the UART: `--uart-in`
`--uart-in STR` feeds STR to UART RX before anything from stdin (escapes `\r`, `\n`, `\t`, `\\`).
**Gotcha:** GIOS ends a token only on a space or a carriage return, never a line feed, so commands need `\r`
(e.g. `--uart-in 'jal 60000000\r'`). Piping `echo jal 60000000` into stdin will hang at the prompt for the same reason.

## Hardware quirks: `--halt-on-quirk`
`--halt-on-quirk[=LIST]` stops the run when the guest does something whose result differs between the real
MIPS150 Verilog CPU (`hardware/src`) and standard MIPS (what Unicorn executes). It prints the PC, the instruction
word and its disassembly, the quirk name, what the hardware would do, and a register dump, then exits with
status **4**. Checks stop *before* the flagged instruction runs. `lb`/`lh` sign extension is not flagged (the Verilog is being fixed to sign-extend like standard MIPS).
`--warn-on-quirk[=LIST]` does the same detection but logs each (quirk, PC) once and keeps running (Unicorn
semantics). Both print a `[quirk] summary` of dynamic hit counts at exit. Without either option nothing changes.

LIST is comma-separated: `all` (the default), category names, and `-name` to exclude
(`--halt-on-quirk=all,-load-use`; a list that starts with `-name` starts from `all`).
| category | flags | hardware behaviour |
|---|---|---|
| `jalr-rd` | `jalr` with rd != `$31` | always links to `$31` |
| `unmapped` | load/store to an undecoded nibble; fetch from an undecoded nibble or MMIO | reads/fetches 0, drops writes |
| `bios-store` | any store into 0x4xxxxxxx | dropped (BIOS is data-read-only) |
| `unaligned` | `lw`/`sw` not 4-aligned, `lh`/`lhu`/`sh` not 2-aligned | loads 0 / drops the store, no AdEL/AdES |
| `lwlr` | `lwl`/`lwr`/`swl`/`swr` | rt <- 0 / store nothing |
| `overflow` | `add`/`addi`/`sub` signed overflow | wraps (no Ov trap) |
| `hilo` | `mult[u]`/`div[u]`/`mfhi`/`mflo`/`mthi`/`mtlo` | no HI/LO: `mf*` write 0, the rest do nothing |
| `syscall` | `syscall` (`break` is not flagged: vvizard uses it as a clean halt) | NOP |
| `uart-lane` | `sb`/`sh` to UART TX (0x80000008) that does not cover byte lane +3 (`sb` at +0xB is fine) | transmits 0x00 |
| `load-use` | the instruction executed right after a load reads that load's rt | no interlock: reads the OLD value |

Memory checks use the same table as everything else (`src/memmap.h`, `vv_canon()`). Cost: one branch per instruction
when off; when on, one 4-byte instruction read and decode per instruction (about 15-20% slower), so `make run` stays
interactive. Details and findings in NOTES.md. `make test-quirks` runs `tests/test_quirks.py`.

## Memory map (`src/memmap.h` is the single table)
The hardware decodes only `ADDR[31:28]`.
| nibble | device | canonical base | size / aliasing |
|---|---|---|---|
| 0x1,0x2,0x3 | DDR (0x1 D$, 0x2 I$, 0x3 both) | 0x10000000 | 256 MiB (frames at 0x10000000+N<<22, GPTEMP 0x12003000) |
| 0x4 | BIOS (data read-only in HW; writes are logged here) | 0x40000000 | 16 KiB |
| 0x5 | D-scratch = SM_BASE_ADDR shared memory (plain RAM) | 0x50000000 | 16 KiB |
| 0x6 | I-scratch | 0x60000000 | 16 KiB |
| 0x8 | MMIO | 0x80000000 | 4 KiB page; registers repeat every 0x100 |
| 0xC | ISR BRAM, PC_ISR=0xC0000180 | 0xC0000000 | 16 KiB |
Any other nibble cannot be decoded. Accessing one raises a TLB-style CPU exception (`UC_ERR_EXCEPTION`).
vvizard reproduces this with Unicorn's virtual-TLB mode and a fill hook, which is why it needs Unicorn 2.1 or newer. Hardware references and the probe findings are in [NOTES.md](NOTES.md).

## MMIO model
- **UART:** TX_READY always reads 1. A write to TX_DATA (offset 0x08, any byte lane) sends byte [7:0] to stdout.
- **UART RX:** RX_VALID and RX_DATA serve any `--uart-in` bytes first, then due `--input-script` bytes, then window keystrokes, then poll stdin without blocking.
- **Counters:** CNT_CYCLE and CNT_INST both return the number of instructions executed since the last CNT_RESET. There is no cycle model.
- **PRNG (0x80000080, `src/prng.c`):** PRNG32LFSR.v, a 32-bit Galois LFSR (mask 0x80200003, reset value 0xACE12468) that steps every clock, not per read. A read in cycle c returns state(c-1) (MemMapIO registers `prng_out` once more before `douta`). Any store (any byte lane, also via the 0x100 aliases) reseeds with the full lane-placed store data, and a seed of 0 becomes 1. The first read in the cycle right after a store returns the old value; from the next cycle on reads return the seed, then the seed advanced one step per cycle. Back-to-back reads return consecutive LFSR states. Cycles are counted as instructions (no stall or cache model), so seeded sequences match hardware only while the CPU does not stall, and the unseeded sequence after reset is not aligned with hardware. CNT_RESET does not reset the PRNG.
- **GPU (stub):** PF_FRAME, GP_FRAME and GP_CODE writes are recorded, and a GP_CODE write logs a GPU "launch". PF_FRAME reads return `{4'h1, frame#, 22'b0}`. GP_STATE reads return 0, meaning idle.
- **Other offsets:** logged as MISS-READ or MISS-WRITE.
- **Interrupts:** none are injected yet, and the CP0 timer does not run (details in NOTES.md).

## Tests
Hex test programs live in `tests/*.hex` (loop, uart, alias, frame, gpcmds/gplaunch with `check_gp.py`, prng). `make test-player` runs `tests/test_player.py` (proj7 player moved in 8 directions + idle via `--input-script` UART keys and injected UARX interrupts). `make test-quirks` runs `tests/test_quirks.py` (one tiny program per `--halt-on-quirk` category, plus clean counterparts). How to run them and their pass conditions are in NOTES.md.

## PixelFeeder display (raylib, `src/display.c`)
- Frame N starts at `0x10000000 + N<<22`. The visible area is 800×600, rows start 4096 bytes apart, and each pixel is a 32-bit big-endian XRGB word.
- A PF_FRAME write (0x80000050) switches frames immediately (hardware waits for the end of the scan; details in NOTES.md).
- Options:
  - `--no-window` runs without a window.
  - `--dump-frame F.ppm` writes the selected frame to F.ppm when the run ends.
  - `--hold` keeps the window open after the run.
  - Closing the window stops emulation.

## Default BIOS (gios)
proj_app* programs are linked against **gios**: `FIXED_ELF ?= ../gios/gios.elf` in `software/proj_app7/Makefile:5`. With bios150v3 loaded instead, proj7 jumps to address 0 because the two BIOS images put different functions at the same addresses.
- By default vvizard loads `VV_DEFAULT_BIOS` raw at 0x40000000. The Makefile sets it from `BIOS ?= $(abspath ../../software/gios/gios.inst.bin)`. If the macro is not defined, the path `<exe dir>/../../software/gios/gios.inst.bin` is used.
- The default BIOS is skipped when any `--load FILE@0x4xxxxxxx` is given. `--bios FILE` overrides the default, and `--no-bios` disables it.
- If the default file is missing, you get a warning and the run continues.
- `make run` always passes `--bios $(BIOS)`.

## GPU command interpreter (`src/gpu.c`)
A write to GP_CODE (0x80000058) runs the GPU on the big-endian command list at that address. It draws into frame `FRAME_BITS(GP_FRAME)` and finishes instantly, so GP_STATE always reads as ready. The GPU-done interrupt (IB_GPU=12) is not raised yet.
| op (bits 31:28) | words | meaning |
|---|---|---|
| 0 STOP | 1 | end of the list |
| 1 FILL | 1 | fill all 800×600 pixels with COLOR28 |
| 2 LINE | 3 | P0, P1: Bresenham, both endpoints included |
| 3 ELIP | 4 | P0 = centre, P1 = (a, b) radii, then XRGB32 fill colour |
| 4 RECT | 4 | P0, P1 = corners: edge colour on the border, fill colour inside, then XRGB32 fill colour |
- A point word is X = bits 25:16 and Y = bits 9:0.
- The interior is filled only when bits 31:24 of the fill colour are non-zero.
- Drawing matches the hardware engines pixel for pixel; see NOTES.md for clipping and ellipse details.
