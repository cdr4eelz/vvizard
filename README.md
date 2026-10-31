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
      --dump-lcd   --screenshot F.png
      --random-ddr[=SEED]   --no-random-ddr   --uart-in STR   --expect REG=VAL
      --quirks=LIST   --no-quirks   --list-quirks   --halt-on-quirk[=LIST]   --warn-on-quirk[=LIST]
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

## Hardware quirks: `--quirks`, `--halt-on-quirk`
vvizard emulates the places where the MIPS150 Verilog CPU (`hardware/src`) does not behave like standard MIPS32
(what Unicorn executes). Each difference is a named quirk that you can turn on or off at run time. By default
vvizard runs the quirks of the **current** hardware. The two bugs fixed in fcdda7f (`jalr-rd`, `lb-sign`) are off
but still available to reproduce old hardware. `bal-untaken` (an RTL bug that is postponed) is on.

    --quirks=LIST        quirks to emulate (default: default)
    --no-quirks          standard MIPS32 (same as --quirks=none)
    --list-quirks        print the quirk table (defaults, hardware behaviour, RTL refs), then exit
    VVIZARD_QUIRKS=LIST  environment variable, applied before --quirks
    make run HW_QUIRKS=LIST   (default: default)

LIST is comma-separated: `default`, `none`, `all`, `NAME`, `-NAME`, `+NAME`. If it starts with `-NAME` or `+NAME`,
it edits the set so far (the defaults, or VVIZARD_QUIRKS). Otherwise it builds a new set.
Examples: `--quirks=-load-use`, `--quirks=+jalr-rd,+lb-sign` (old hardware), `--quirks=none,hilo`.
`uart-lane` is accepted as an old name for `mmio-lane`.

Quirks, all on by default: `bal-untaken`, `load-use`, `unmapped`, `unmapped-fetch`, `bios-store`, `ddr2`,
`unaligned`, `lwlr`, `overflow`, `hilo`, `syscall`, `reserved`, `cop0`, `mmio-lane`. Off by default (fixed in
hardware): `jalr-rd`, `lb-sign`. **[QUIRKS.md](QUIRKS.md)** has the full list. For each quirk it gives the hardware
behaviour against MIPS32, the RTL file:line evidence, its status, how it was confirmed (hwprobe check numbers),
and its test coverage. It also lists the differences that are not emulated. `./vvizard --list-quirks` prints the
short form.

**Detection.** `--halt-on-quirk[=LIST]` stops when the guest hits a quirk. It prints the PC, the instruction and
its disassembly, the quirk name, the hardware behaviour and a register dump, then exits with status **4**. It
stops *before* the instruction runs. With no LIST, it covers the quirks being emulated. A LIST starting with
`-NAME` removes from that set, and `all` also reports quirks that aren't emulated (for example, `jalr` with rd
!= `$31`). `--warn-on-quirk[=LIST]` logs each (quirk, PC) once, tagged `[emulated]` or `[not emulated: standard
MIPS]`, and keeps running. Both print a `[quirk] summary` of hit counts at exit. `make run` passes
`QUIRKS=--halt-on-quirk` by default.

**Cost.** With all quirks off and no detection, the extra cost is one branch per instruction. Otherwise each
instruction costs one 4-byte read and a decode: about 30% slower at -O0 (GIOS+proj7, 20M instructions: 4.4 s
with `--no-quirks`, 5.8 s by default). Current GIOS, proj7 and isr7 hit no quirks in that run. See QUIRKS.md for the
evidence and NOTES.md for the emulation design.

## Memory map (`src/memmap.h` is the single table)
The hardware decodes only `ADDR[31:28]`.
| nibble | device | canonical base | size / aliasing |
|---|---|---|---|
| 0x1,0x2,0x3 | DDR (0x1 D$, 0x2 I$, 0x3 both) | 0x10000000 | 256 MiB (frames at 0x10000000+N<<22, GPTEMP 0x12003000) |
| 0x4 | BIOS (data read-only in HW; writes are logged here) | 0x40000000 | 16 KiB |
| 0x5 | D-scratch = SM_BASE_ADDR shared memory (plain RAM) | 0x50000000 | 16 KiB |
| 0x6 | I-scratch | 0x60000000 | 16 KiB |
| 0x8 | MMIO (MemMapIO, `addra = ADDR[13:2]`) | 0x80000000 | 16 KiB, repeats every 0x4000. Registers only at 0x80000000-0x800000FC, LCD at 0x80000100-0x8000017C, the rest reads 0 and drops stores (see MMIO model) |
| 0xC | ISR BRAM, PC_ISR=0xC0000180 | 0xC0000000 | 16 KiB |
Any other nibble can't be decoded. In hardware a load from one reads 0, a store is dropped and a fetch runs nops.
vvizard emulates this through the `unmapped` and `unmapped-fetch` quirks, which map a zero "VOID" page. With
those quirks off, the access raises a TLB-style CPU exception (`UC_ERR_EXCEPTION`). vvizard does this with Unicorn's virtual-TLB mode and a fill hook, which is why it needs Unicorn 2.1 or newer. Hardware references and the probe findings are in [NOTES.md](NOTES.md).

## MMIO model
MemMapIO gets the word address `addra = ADDR[13:2]` and now decodes it (cs150 branch `lcd-text`, `cpu/MemMapIO.v`):
| offset (`ADDR[13:0]`) | `addra` | device |
|---|---|---|
| 0x0000-0x00FC | `addra[11:6] == 0`, compared on `addra[5:0]` | the registers below (UART, counters, PF/GP, PRNG) |
| 0x0100-0x017C | `addra[11:5] == 2` | 2x16 LCD text buffer, write-only |
| 0x0180-0x3FFC | - | nothing: reads 0 here (`32'dx` in HW), stores dropped, logged as MISS-READ / MISS-WRITE |

The registers no longer repeat every 0x100: 0x80000108 is LCD char 2, not UART TX data, and 0x80000180 or 0x80000280
are not the PRNG. `ADDR[27:14]` is still not decoded, so the whole 16 KiB map repeats (0x80004008 is UART TX data).
This is plain address decode, not a quirk: the `unmapped` quirk covers undecoded nibbles only, and MMIO misses read 0
and drop stores in every quirk setting.
- **UART:** TX_READY always reads 1. A write to TX_DATA (offset 0x08) sends byte [7:0] of the written word to stdout. With `mmio-lane` (the default), that's the lane-placed word, so only `sw` and `sb` at +0xB send the byte.
- **UART RX:** RX_VALID and RX_DATA serve any `--uart-in` bytes first, then due `--input-script` bytes, then window keystrokes, then poll stdin without blocking.
- **LCD (0x80000100-0x8000017C, `src/lcd.c`):** LCDText2x16.v's 32-character buffer behind `LCDText2x16_MemPort`. Character i (0-15 line 1, 16-31 line 2, left to right) is at `0x80000100 + 4*i` and takes `dina[7:0]` of any store (any byte mask). With `mmio-lane` (the default), that's the lane-placed word, so `sw` and `sb` at +3 (`sh` at +2) write the byte, and `sb` at +0..+2 (`sh` at +0) writes 0x00, as in hardware. Without the quirk the char is the stored value's low byte. Reads return 0 and have no side effects. Reset contents are LCDText2x16's `INIT_TEXT` default, 32 spaces. `--dump-lcd` prints the buffer (text and hex) at exit, and the window shows it in the status strip.
- **Counters:** CNT_CYCLE and CNT_INST both return the number of instructions executed since the last CNT_RESET. There is no cycle model.
- **PRNG (0x80000080, `src/prng.c`):** PRNG32LFSR.v, a 32-bit Galois LFSR (mask 0x80200003, reset value 0xACE12468) that steps every clock, not per read. A read in cycle c returns state(c-1) (MemMapIO registers `prng_out` once more before `douta`). Any store (any byte lane, also via the 16 KiB alias 0x80004080; 0x80000180 is no longer an alias) reseeds with the full lane-placed store data, and a seed of 0 becomes 1. The first read in the cycle right after a store returns the old value; from the next cycle on reads return the seed, then the seed advanced one step per cycle. Back-to-back reads return consecutive LFSR states. Cycles are counted as instructions (no stall or cache model), so seeded sequences match hardware only while the CPU does not stall, and the unseeded sequence after reset is not aligned with hardware. CNT_RESET does not reset the PRNG.
- **GPU (stub):** PF_FRAME, GP_FRAME and GP_CODE writes are recorded, and a GP_CODE write logs a GPU "launch". PF_FRAME reads return `{4'h1, frame#, 22'b0}`. GP_STATE reads return 0, meaning idle.
- **Other offsets:** logged as MISS-READ or MISS-WRITE (reads 0, stores dropped).
- **Interrupts:** `--irq` injects only UART-RX interrupts. The COP0 timer (Count == Compare) and RTC interrupts are not raised, although `mfc0 Count` counts instructions with the `cop0` quirk (details in NOTES.md).

## Tests
`make test` runs `check-hex`, `test-hex`, `test-quirks` and `test-player`. `make test-hex` runs `tests/test_hex.py`
(loop, uart, lcd, alias, frame, gpcmds/gplaunch with `check_gp.py`, prng, hwprobe and the hardware-timing / bug-detector
programs). `make test-player` runs `tests/test_player.py` (proj7 player moved in 8 directions + idle via
`--input-script` UART keys and UARX interrupts). `make test-quirks` runs `tests/test_quirks.py`: one tiny program per
quirk for detection, with clean counterparts, each quirk on and off against the standard MIPS result, and the CLI and
environment interface. How to run them and their pass conditions are in NOTES.md.

**Test programs are built from assembly.** Every test's machine code is a `.s` file: `tests/*.s` (the programs above)
and `tests/quirk/det_*.s` / `emu_*.s` (the quirk snippets). `make tests` (a dependency of the test targets) assembles
them with the MIPS GNU toolchain found on the PATH (`mips-linux-gnu-*`, else `mips64-linux-gnu-*`, or `make M_PRE=...`)
into `build/tests/NAME.bin` (raw big-endian `.text`) and `NAME.hex` (one word per line). The settings follow cs150
`software/makefile.in` (`ASOPTS`) and `makefile.isr`: `-EB -mabi=32 -non_shared -mno-shared -G0 -mno-pdr --no-break
--no-trap`, static link with `-T tests/test.ld` (`.text` only, no build-id), `objcopy -O binary -j .text`. Each `.s`
names its link/load address and ISA in its header (`# vv-load: 0x60000000`, `# vv-isa: -mips32r2`; the programs use
`-mips1` like cs150 except hwprobe and the quirk snippets, which need MIPS32r2 encodings). `tests/check.inc` holds the
CHECK/BEGIN_CHECKS/END_CHECKS/LA_CODE macros used by hwprobe (the same instruction sequences as the cs150 Icarus
`check.inc`, so vvizard no longer needs the cs150 tree to build them).

The committed `tests/**/NAME.hex` files are a fallback for machines without the toolchain: `tests/vvprog.py` uses
`build/tests/NAME.bin` when it exists and is newer than the `.s`, else converts the `.hex`. `make check-hex` verifies
the built programs are byte-identical to the committed hex (part of `make test`; skipped without a toolchain) and
`make refresh-hex` copies the built hex over the fallbacks after an intentional change.

## PixelFeeder display (raylib, `src/display.c`)
The window is titled `vvizard`. It shows the selected frame unscaled at the top (800×600) and a 76-pixel status strip
underneath:
- **Left:** the 2x16 LCD in green dot-matrix style. Codes 0x20-0x7F use HD44780 ROM A00 glyphs (`src/lcdfont.h`, so 0x5C is ¥ and 0x7E/0x7F are → / ←). 0x10-0x1F and 0x80-0xA0 are blank as in A00. CGRAM codes 0x00-0x0F (LCDText2x16 never loads CGRAM) and 0xA1-0xFF (katakana and symbols, not drawn) show as a hollow box.
- **Right, line 1:** run state (RUN, STOPPED (break / max-insns / error) or HALTED (quirk)), instructions, the displayed PF_FRAME and the number of PF_FRAME writes, GP launches and injected IRQs.
- **Right, line 2:** when D-scratch holds proj6/proj7 shared memory (magic 0xFEDBEEFn at 0x50000000): graphics paused (flags bit 0, ' ' key), clock paused (flags bit 1, 'd'/'e' keys) and the BCD mm:ss clock.
- **Right, bottom:** recent keys. Window keystrokes are shown when pressed, `--input-script` keys when due, and `--uart-in` and stdin bytes when the guest reads them. The newest key is at the right and older keys scroll out to the left.
- `--screenshot F.png` writes the whole window, strip included, as PNG when the run ends (needs a window; works under `xvfb-run`).

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
