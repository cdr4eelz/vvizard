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
    make run        # = ./vvizard: boot GIOS and auto-start proj7 (see "Running: defaults and make targets")
    make DEBUG=1    # unoptimized debug build (-O0 -g3 -fno-omit-frame-pointer)

- vvizard itself is built with `-O3 -g` by default (`OPTFLAGS`, or replace `CFLAGS` entirely). `DEBUG=1` selects the
  old debug flags. `build/.flags` records the compile/link flags, and every object is rebuilt when they change, so
  switching between the two never mixes objects. The MIPS assembler settings for the test programs (`T_ASOPTS`) are
  separate and unchanged.
- The default program comes from the cs150 software tree, so build GIOS first (`make` in `software/gios`), then
  `software/proj_app7` and `software/draw`.
- Big-endian is the default CPU mode. Pick one at run time with `--little` or `--big`.
- A Unicorn in `~/.local` takes priority automatically (PKG_CONFIG_PATH and rpath are set in the Makefile, and
  the `make run*` targets also set `LD_LIBRARY_PATH`).

## Running: defaults and make targets
All settings are command-line arguments; the Makefile no longer holds any. **`./vvizard` with no arguments runs the
proj7 player**, exactly what `make run` used to do. Every option takes `--opt=VALUE` or `--opt VALUE`
(`./vvizard --help` lists them all).

**Default program.** The software tree SW is `--sw-dir DIR`, else `<directory of the vvizard executable>/../../software`
(the executable's real path: `/proc/self/exe` on Linux, `_NSGetExecutablePath` on macOS), so with the cs150 layout
`<project>/extern/vvizard/vvizard` + `<project>/software` it works from any working directory. All images are loaded as
raw bytes:

| image | loaded at |
|---|---|
| `SW/gios/gios.inst.bin` | 0x40000000 (BIOS; the run starts here, `--entry 0x40000000`) |
| `SW/draw/draw.inst.bin` | 0x50000000 (D-scratch) |
| `SW/proj_app7/isr7.bin` | 0xC0000000 (ISR BRAM) |
| `SW/proj_app7/proj7.inst.bin` | 0x60000000 (I-scratch) |
| autorun `jal 60000000\r` | typed at the GIOS prompt (as `--uart-in`) |

If one of these files is missing, vvizard says which and exits 1.
- **Giving a program replaces the default program**: any `--load FILE@ADDR` or ELF file argument means no default
  loads and no autorun. The BIOS (`SW/gios/gios.inst.bin`) is still loaded unless a `--load` targets 0x4xxxxxxx, or
  `--bios FILE` / `--no-bios` says otherwise; a missing BIOS is then only a warning. This keeps every existing command
  line that names its own program (all the tests) running just that program.
- `--bare`: only the BIOS, no loads, no autorun (an interactive GIOS prompt).
- `--uart-in STR` replaces the autorun; `--no-autorun` drops it.

**Default settings** and how to turn each off:

| default | off / change |
|---|---|
| unlimited instructions | `--max-insns N`, `--max-seconds S` |
| frame ends every 828960 emulated cycles (`--frame-clock=emulated`) | `--frame-clock=wall`, `--pf-period N` |
| COP0150 interrupts (`--irq`) | `--no-irq` |
| random DDR, seed from the time | `--random-ddr=SEED`, `--no-random-ddr` |
| window, window keys to UART | `--no-window`, `--no-window-keys` |
| MMIO accesses not logged (`--quiet-mmio`) | `--mmio-log` (= `--no-quiet-mmio`) |
| `--quirks=default` (current hardware) | `--quirks=LIST`, `--no-quirks` |
| `--halt-on-quirk` (emulated set) | `--halt-on-quirk=LIST`, `--warn-on-quirk[=LIST]`, `--no-halt-on-quirk` |
| `--color=auto` | `--color=always`, `--color=never` |

Before this change the built-in defaults were the test-oriented ones (1,000,000 instruction limit, MMIO log on, no
halting); command lines that relied on them now say so (`--max-insns 1000000 --no-halt-on-quirk --mmio-log`).
Options are applied left to right, so a later one wins (`--no-halt-on-quirk --halt-on-quirk=all` halts).

**make targets** (all pass `EXTRA=` through, e.g. `make run EXTRA="--no-window --max-insns 200000"`):

| target | runs |
|---|---|
| `make run` | `./vvizard` (the defaults above) |
| `make run-std` | `--quirks=none`: standard MIPS32 (nothing emulated, so nothing to halt on; guest-bug detectors still warn) |
| `make run-legacy-hw` | `--quirks=+jalr-rd,+lb-sign`: the hardware before fcdda7f |
| `make run-warn` | `--warn-on-quirk`: log each quirk hit once and keep running |
| `make run-debug` | `--mmio-log --warn-on-quirk=all`: every MMIO access and GP launch, and all quirks (also non-emulated) |
| `make run-noirq` | `--no-irq` |
| `make run-gios` | `--bare`: GIOS prompt only |

The old variables map to arguments: `BIOS=F` -> `--bios F`, `AUTORUN=` -> `--no-autorun`, `HW_QUIRKS=L` -> `--quirks=L`,
`QUIRKS=` -> `--no-halt-on-quirk`, `COLOR=C` -> `--color=C`, `IMEM_APP=...`/`BIN=...` -> spell the `--load`s out.

**Argument parsing: `getopt_long`.** vvizard keeps the C library's `getopt_long` rather than vendoring a parser.
It is in glibc and in the macOS (BSD) libc, so it needs nothing extra on Ubuntu, Debian or macOS (unlike glibc-only
`argp`). It already handles `--opt=VALUE`, `--opt VALUE`, unique-prefix abbreviations and options mixed with file
arguments. The one gap, `--opt VALUE` for options whose value is optional (`--color`, `--trace`, `--random-ddr`,
`--halt-on-quirk`, `--warn-on-quirk`), is a 5-line helper in `src/main.c`: it takes the next word only if it is a valid
value for that option, so `--trace prog.elf` still treats `prog.elf` as a program. A vendored single-file library
such as cofyc/argparse would add a dependency and a license file without covering more (it has no optional-value
options and its `--no-` handling is per-flag anyway).

## Output streams and color (`--color`)
**stdout carries only the bytes the guest sends on the UART (TX).** Everything vvizard prints for itself goes to
**stderr**: `[stop]`, `[quirk]`, `[irq]`, `[hw]`, `[mmio]`, `[trace]`, register dumps, summaries, load messages,
errors, and raylib's log (prefixed `[raylib]`). So `./vvizard ... >uart.txt` captures only the serial output, and
`2>/dev/null` hides vvizard's diagnostics. (`--help` and `--list-quirks` print to stdout, since you asked for them.)
UART text is flushed at each newline, every byte when stdout is a terminal, before any stderr output (so a
`2>&1` log keeps its order), and every 4096 instructions (so a prompt with no newline shows up).

`--color[=WHEN]` with WHEN = `auto` (the default), `always` (also bare `--color`) or `never`:
- `auto` turns color on when stdout is a terminal, unless `NO_COLOR` is set (non-empty) or `TERM=dumb`.
- With color on, UART text on stdout is bold phosphor green (xterm-256 color 46 when `TERM` has `256color` or
  `COLORTERM` is set, else bold green). The color is reset at each line end, before any stderr output, at exit, and on
  SIGINT/SIGTERM/SIGHUP/SIGQUIT.
- stderr is highlighted only when color is on **and** stderr is a terminal (with `always`, it's highlighted regardless).
  The leading `[tag]` of each line gets a per-category color (`[stop]` red, `[quirk]` yellow, `[irq]` magenta,
  `[hw]` cyan, `[mmio]` blue, ...), `[guest bug]` / `PASS` / `FAIL` markers are colored, and register names in
  register dumps (`zero`, `t0`, `pc`, `hi`, ...) and `--trace-regs` lines are highlighted.

**Keyboard:** with the window focused, typed keys go to UART RX: printable ASCII as-is, Enter -> `\r`, Backspace -> 0x08,
Tab -> `\t`. **Esc ends the run** (it is not sent to the UART, since a lone Esc doesn't travel well over the real UART
with `screen`): `[stop] Esc pressed` on stderr, then the same shutdown and exit status (0) as closing the window. It works
with `--no-window-keys` too and also ends `--hold`. Terminal stdin still works too.
proj7: `u i o / j k l / m , .` move, space pauses, `d`/`e` clock. `--no-window-keys` disables window capture.

**Note:** `bench` and `draw` are linked to run at DDR 0x10000000 (`LDSTYLE := inject_cache`). Their raw bytes can be
placed in the scratchpads, but they won't run correctly from there unless relinked. proj7 (0x60000000) and isr7
(0xC0000000) are linked for where they're loaded. Also, the default `draw.inst.bin` load overwrites the start of D-scratch, which proj7
uses as its shared memory (the stack is at the top of D-scratch).

## Usage
    ./vvizard [opts] [isr.elf ...] [prog.elf]      # entry = last ELF's e_entry; no program = the default program
      --bare  --sw-dir DIR  --bios FILE  --no-bios  --uart-in STR  --no-autorun
      --load FILE@ADDR  raw binary or ELF (repeatable)   --entry ADDR   --sp ADDR (default: not preset)
      --max-insns N (default 0 = unlimited)  --max-seconds S  --until ADDR  --trace[=N]  --trace-regs  --quiet-mmio  --mmio-log
      --little | --big   --window | --no-window   --no-window-keys   --dump-frame F.ppm   --hold
      --dump-lcd   --screenshot F.png
      --random-ddr[=SEED]   --no-random-ddr   --expect REG=VAL   --color[=WHEN]
      --quirks=LIST   --no-quirks   --list-quirks   --halt-on-quirk[=LIST]   --warn-on-quirk[=LIST]   --no-halt-on-quirk
      --quiet-quirk=LIST   --irq | --no-irq   --legacy-io   --uart-rx-gap N   --pf-period N   --frame-clock=emulated|wall
      --no-guest-exit   --threads=on|off (--sync = --threads=1)   --event-trace F
A `break` instruction stops the run cleanly. When the run ends or fails, the registers are printed.
`--expect` sets the exit status: 0 if every check passes, 3 if any fails.

**Exit status:** 0 normal stop (`break`, `--max-insns`, `--max-seconds`, `--until`, window closed, Esc); 1 setup or CPU error; 2 usage;
3 an `--expect` failed; 4 `--halt-on-quirk` stopped; a guest exit-register store: the stored code & 0xFF (see below).

### Ending a run from guest code
A store to the **guest exit register at 0x80000084** (one word past the PRNG at 0x80000080) stops vvizard right after
the store instruction. It prints `[exit] guest exit code N (0x...) at pc=... -> exit status S` on stderr, flushes the
UART output, runs the normal shutdown (summaries, registers, `--dump-frame`, `--dump-lcd`, `--screenshot`, `--hold`)
and exits with **status = code & 0xFF** (only the low 8 bits reach the shell, so -1 is 255 and 256 is 0; codes 1-4
look like vvizard's own statuses, and the `[exit]` line tells them apart; a failing `--expect` still gives 3).

On the FPGA the address is free: MemMapIO.v decodes registers only at `addra[5:0]` = 0x00-0x06, 0x14-0x17 and 0x20
(PRNG), so 0x84 (`addra` 0x21) selects nothing and the store is a no-op. 0x80000084-0x800000FC is the **extended area**
for vvizard-only registers. So guest code stores its code, then does what the FPGA needs: interrupts off and spin.
`--no-guest-exit` makes vvizard behave like the FPGA (the store is ignored, once noted on stderr). Use `sw`: an `sb`
at 0x80000087 or `sh` at 0x80000086 also works (the byte lands in the low lane), but `sb` at 0x80000084 puts the
code in bits 31:24 (status 0, and the default `mmio-lane` halt). A read returns 0 (HW: undefined) with no effect.

Assembly (`.set noreorder`):

    lui   $t0, 0x8000
    li    $t1, 42               # exit code
    sw    $t1, 0x84($t0)        # vvizard stops here (VV_EXIT)
    mfc0  $t2, $12              # FPGA (or --no-guest-exit) goes on: Status.IE = 0 ...
    nop
    li    $t3, ~1
    and   $t2, $t2, $t3
    mtc0  $t2, $12
1:  b     1b                    # ... and spin
    nop

C:

    #define VV_EXIT (*(volatile unsigned *)0x80000084)
    static void vv_exit(unsigned code) {
        VV_EXIT = code;                          /* vvizard stops here */
        __asm__ __volatile__("mfc0 $t0, $12\n\tnop\n\tli $t1, ~1\n\t"
                             "and $t0, $t0, $t1\n\tmtc0 $t0, $12" ::: "t0", "t1");   /* FPGA: IE = 0 */
        for (;;) ;                               /* ... and spin */
    }

### Random DDR at startup
DDR (256 MiB at 0x10000000) is filled with random data before anything is loaded, as real DRAM would be at power-up.
It's on by default, and every run prints its seed, e.g. `[ddr] random fill 256 MiB, seed=1792706903`.
Repeat a run with `--random-ddr=SEED`, or start with zeroed DDR using `--no-random-ddr`. Loaded images overwrite the
random data where they land, and frame buffers start out as noise until something draws into them.

### Typing into the UART: `--uart-in`
`--uart-in STR` feeds STR to UART RX before anything from stdin (escapes `\r`, `\n`, `\t`, `\\`). Each byte takes
the real 115200-baud frame time (4340 cycles), and the next `--uart-in` byte is only sent after the guest has read the
previous one plus `--uart-rx-gap` idle cycles, like someone typing at the prompt, so auto-typing never overruns RX.
Script keys, window keys and stdin are not held back (see [Hardware timing](#hardware-timing-and-interrupts)).
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

LIST is comma-separated and edits the set so far (the defaults, then VVIZARD_QUIRKS, then each `--quirks` in order)
left to right: `NAME` or `+NAME` adds, `-NAME` or `no-NAME` removes, and `none`, `all` and `default` replace the set.
Examples: `--quirks=no-load-use` (= `-load-use`), `--quirks=jalr-rd,lb-sign` (= `+jalr-rd,+lb-sign`, old hardware),
`--quirks=none,hilo` (only `hilo`). (Before, a list starting with a plain NAME built a new set; write `none,NAME` for that.)
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
`-NAME`/`no-NAME` removes from that set (`none,NAME` = only NAME), and `all` also reports quirks that aren't emulated (for example, `jalr` with rd
!= `$31`). `--warn-on-quirk[=LIST]` logs each (quirk, PC) once, tagged `[emulated]` or `[not emulated: standard
MIPS]`, and keeps running. Both print a `[quirk] summary` of hit counts at exit. The guest-bug detectors (below) always warn unless
`--quiet-quirk` silences them, and `--halt-on-quirk` also stops on them. **`--halt-on-quirk` is on by default**;
`--warn-on-quirk` keeps running, `--no-halt-on-quirk` turns detection of emulated quirks off.

**Cost.** Emulated quirks and the decode-based detectors (undef-reg, dcache-stale) cost a 4-byte read from guest
RAM and a decode per instruction; tx-busy, rx-overrun, irq-storm and unmapped-fetch are detected in the device
model and cost (almost) nothing per instruction. GIOS+proj7 at -O3 on the Debian 13 box: about 22M instr/s with
the defaults, 35M with only the UART/irq-storm detectors, 38M with nothing emulated or detected (NOTES.md
"Performance"). GIOS, proj7 and isr7 of Oct 28 hit no quirks (a later isr7 hits `load-use`, NOTES.md "Finding:
isr7 load-use"). See QUIRKS.md for the evidence and NOTES.md for the emulation design.

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
| 0x0000-0x00FC | `addra[11:6] == 0`, compared on `addra[5:0]` | the registers below (UART, counters, PF/GP, PRNG); 0x84-0xFC is the extended area (no RTL register; 0x84 = vvizard's guest exit register) |
| 0x0100-0x017C | `addra[11:5] == 2` | 2x16 LCD text buffer, write-only |
| 0x0180-0x3FFC | - | nothing: reads 0 here (`32'dx` in HW), stores dropped, logged as MISS-READ / MISS-WRITE |

The registers no longer repeat every 0x100: 0x80000108 is LCD char 2, not UART TX data, and 0x80000180 or 0x80000280
are not the PRNG. `ADDR[27:14]` is still not decoded, so the whole 16 KiB map repeats (0x80004008 is UART TX data).
This is plain address decode, not a quirk: the `unmapped` quirk covers undecoded nibbles only, and MMIO misses read 0
and drop stores in every quirk setting.
- **UART TX:** a write to TX_DATA (offset 0x08) sends byte [7:0] of the written word to stdout (the only thing written to stdout; see [Output streams](#output-streams-and-color---color)). With `mmio-lane` (the default), that's the lane-placed word, so only `sw` and `sb` at +0xB send the byte. TX_READY reads 0 for 4773 cycles after an accepted byte (start + 8 data + stop at 115200 baud, 50 MHz), and a TX_DATA write while it is 0 is **lost**, as in MemMapIO.v (the `tx-busy` guest bug). When it goes back to 1, Cause bit 11 latches.
- **UART RX:** host bytes come from `--uart-in` first, then due `--input-script` bytes, then window keystrokes, then stdin (polled without blocking). Each byte lands 4340 cycles after its start bit, in a single holding register: RX_VALID is 1 while a byte is held (and 0 while the next one is being shifted in), an RX_DATA read returns it and clears RX_VALID, and a byte that lands before the last one was read overwrites it (the `rx-overrun` guest bug). Each landed byte latches Cause bit 10.
- **LCD (0x80000100-0x8000017C, `src/lcd.c`):** LCDText2x16.v's 32-character buffer behind `LCDText2x16_MemPort`. Character i (0-15 line 1, 16-31 line 2, left to right) is at `0x80000100 + 4*i` and takes `dina[7:0]` of any store (any byte mask). With `mmio-lane` (the default), that's the lane-placed word, so `sw` and `sb` at +3 (`sh` at +2) write the byte, and `sb` at +0..+2 (`sh` at +0) writes 0x00, as in hardware. Without the quirk the char is the stored value's low byte. Reads return 0 and have no side effects. Reset contents are LCDText2x16's `INIT_TEXT` default, 32 spaces. `--dump-lcd` prints the buffer (text and hex) at exit, and the window shows it in the status strip.
- **Counters:** CNT_CYCLE and CNT_INST both return the number of instructions executed since the last CNT_RESET. Time in vvizard is one cycle per instruction (no stall or cache timing).
- **PRNG (0x80000080, `src/prng.c`):** PRNG32LFSR.v, a 32-bit Galois LFSR (mask 0x80200003, reset value 0xACE12468) that steps every clock, not per read. A read in cycle c returns state(c-1) (MemMapIO registers `prng_out` once more before `douta`). Any store (any byte lane, also via the 16 KiB alias 0x80004080; 0x80000180 is no longer an alias) reseeds with the full lane-placed store data, and a seed of 0 becomes 1. The first read in the cycle right after a store returns the old value; from the next cycle on reads return the seed, then the seed advanced one step per cycle. Back-to-back reads return consecutive LFSR states. Cycles are counted as instructions (no stall or cache model), so seeded sequences match hardware only while the CPU does not stall, and the unseeded sequence after reset is not aligned with hardware. CNT_RESET does not reset the PRNG.
- **PixelFeeder:** a PF_FRAME write only queues the frame. It is displayed from the end of the current scan frame (every 828960 cycles: VESA 800x600@60, 1056x628 pixel clocks at 40 MHz = 16.579 ms; or every 16.579 ms of host time with `--frame-clock=wall`), when Cause bit 13 latches. PF_FRAME reads return `{4'h1, frame#, 22'b0}` of the frame being displayed, not the last one written, so software that waits for its frame (proj7 `pf_wait`) now really waits.
- **GPU:** a GP_CODE write runs the command list at once (`src/gpu.c`; no GPU busy time) and latches Cause bit 12. GP_STATE reads return `{pf_status, gp_status}` in the `graphics.h` `gstate_s` layout: displayed frame in bits 29:24, `gp_fault` in bit 15 (set by a bad opcode, cleared by the next launch), the launched frame in bits 13:8, and all five engines ready (bits 4:0). That is the intended layout; GPU.v as written drops the fault bit and puts the frame in bits 14:9, an RTL bug (see QUIRKS.md "gp-status RTL bug").
- **Guest exit (0x80000084, vvizard only):** a store ends the run with that exit code; see [Ending a run from guest code](#ending-a-run-from-guest-code).
- **Other offsets:** logged as MISS-READ or MISS-WRITE (reads 0, stores dropped).
- **Interrupts:** see the next section.

## Hardware timing and interrupts
`src/hw.c` models the I/O devices against time (one cycle per instruction) from the RTL on the cs150 ISE branch:
| source | Cause bit | when |
|---|---|---|
| UART RX | 10 | a received byte lands (rising edge of RX valid) |
| UART TX | 11 | TX_READY goes back to 1 after a byte |
| GPU | 12 | a GP_CODE launch completes (at once in vvizard) |
| PixelFeeder | 13 | end of each scan frame (828960 cycles, or 16.579 ms wall time with `--frame-clock=wall`) |
| RTC | 14 | Count == 0xFFFFFFFF |
| timer | 15 | Count == Compare (Compare is 0xFFFF at reset, so this fires once ~65536 cycles in) |

As in COP0150.v, a Cause bit stays set until software clears it with `mtc0 Cause` (`mtc0` can only clear these bits),
writing Compare clears bit 15, and the bits latch whatever Status.IM says. Interrupts are taken by default (`--no-irq`
turns that off): when Status.IE is set and `Status.IM & Cause.IP` is non-zero, vvizard sets EPC to the next PC, clears
IE and jumps to the ISR at 0xC0000180. As in CPUMIPS.v, it waits while the instruction just run was an `mtc0` or a
branch, or was a taken branch's target, so after the cs150 ISR return (`jr $k0; mtc0 Status`) two instructions of the
interrupted code always run before the next entry, and a loop like `1: b 1b; nop` is never interrupted. The ISR returns the cs150
way (`jr $k0` with `mtc0 Status` in the delay slot; `eret` is a no-op on the MIPS150).

Registers start at 0 and are not preset (RegFile.v has no reset): GIOS sets `$sp` itself. `--sp ADDR` presets `$sp` for
programs run without GIOS.

    --legacy-io        the old untimed model: TX always ready, RX bytes on demand with a level-style UARX interrupt,
                       PF_FRAME switches at once, no TX/GPU/PF/timer/RTC interrupts
    --uart-rx-gap N    idle cycles the host leaves between RX bytes (default 4340, one byte time; 0 = back to back)
    --pf-period N      cycles per scan frame (default 828960; 0 = PF_FRAME writes take effect at once, no bit 13)
    --frame-clock=MODE wall (default): a frame ends every 16.5792 ms of host monotonic time (60.317 frames
                       per wall second); emulated: every --pf-period cycles (deterministic; the tests use it)
    --no-irq           don't take interrupts (the Cause bits still latch)

**Frame clock.** The hardware scans VESA 800x600@60: 1056x628 pixel clocks at 40 MHz = 663168 clocks = 16.5792 ms
(60.317 Hz), which is 828960 cycles of the 50 MHz CPU (`VV_PF_PERIOD`, `src/hw.h`). Until Oct 31 2026 the default
was 865800 (1040x666 at 40 MHz, as the DVI instance in `ml505top.v:184-185` is written; `--pf-period 865800` gives
the old timing). With `--frame-clock=emulated` frame ends come every 828960 instructions, so every run is
repeatable and the guest gets the hardware's work per frame, but at vvizard's speed (10-20M instructions/s against
50M) that is only ~20 frames per wall second and games look slow. The default, `--frame-clock=wall` (since Oct 31 2026, the user's choice), instead ends a frame (PF_FRAME
switch, Cause bit 13, window redraw) each time host `CLOCK_MONOTONIC` passes the next 16.5792 ms deadline, checked
every 1024 instructions (~0.05-0.1 ms). Deadlines missed while the host stalls are skipped, not caught up, so frames
never burst; the `[frame]` line counts them. The guest then gets ~instr/s / 60 instructions per frame (~290k on the
Debian box instead of 828960). Only the frame clock changes: Count/Compare, the RTC, the UART and the PRNG still run
on emulated cycles, so e.g. proj7's mm:ss clock (timer driven) still runs at emulated speed. Wall runs are not
repeatable (the tests use `--frame-clock=emulated` explicitly). vvizard has no pause of its own; proj7's graphics /
clock pause (the status strip's PAUSED) is guest state and doesn't stop the PixelFeeder, which keeps scanning on
hardware too.

The `[frame]` line at exit gives the frame clock, frames scanned, wall time, frames per wall second and the ratio
to 60.317, instructions per second and per frame. `--max-seconds S` ends a run after S seconds of wall time (e.g. to
measure that).

The `[hw]` line at exit counts UART bytes sent, lost, received and overrun, frames scanned, the Cause bits latched per
source, and interrupt storms. Not modelled: GPU busy time, cache and stall timing (so the PixelFeeder frame phase and
any cycle counts are approximate), I-cache staleness.

## Guest-bug detectors
Beyond the CPU quirks, vvizard flags five guest software bugs. They **warn by default** (one line per site, tagged
`[guest bug]`), stop with `--halt-on-quirk`, and are silenced with `--quiet-quirk=NAME,...`:
| name | the guest... | on hardware |
|---|---|---|
| `tx-busy` | writes UART TX_DATA while TX_READY is 0 | the byte is lost |
| `rx-overrun` | leaves an RX byte unread until the next one lands | the old byte is overwritten |
| `irq-storm` | returns from the ISR (IE back on, or `eret`) with the Cause bit that caused the interrupt still set and enabled | the ISR is entered again two instructions later, over and over |
| `undef-reg` | reads a GPR never written since reset (including `$sp` without `--sp`) | 0 after configuration, leftovers after a reset |
| `dcache-stale` | loads through the D-cache from a line the GPU has since written in DDR | the load returns the old cached data |

`dcache-stale` follows Cache.v: direct-mapped, 256 lines of 32 bytes, write-through without write-allocate, filled on
a read miss, never flushed or snooped. By default vvizard also returns the stale data, as the hardware does
(`--quirks=-dcache-stale` returns DDR). Details per detector are in [QUIRKS.md](QUIRKS.md#guest-bug-detectors).

## Threads (`--threads`, `--sync`)
By default (`--threads=on`) the GPU draws command lists on a worker thread, and with a window the emulator runs
on its own thread while the main thread draws the window (the frame is copied into a triple buffer at each frame
end; raylib stays on the main thread). The guest then sees a busy GPU: GP_STATE's ready bits read 0 from the
`GP_CODE` store until `launch + pixels/4 + 32 x commands` cycles, Cause bit 12 latches at that edge, and a
`GP_CODE` store while busy is dropped (the `gp-busy` guest bug; wait with `GP_WAIT()`). The busy time is fixed at
the launch and the emulator waits for the worker at that cycle if needed, so threaded runs with
`--frame-clock=emulated` are repeatable (also windowed vs headless). `--sync` (= `--threads=1` / `--threads=off`) is
the old single thread: the GPU finishes inside the store and is always ready; `make test` uses it, `make
test-threads` covers the threaded mode. Speed (proj7, NOTES.md "Threads"): windowed 1.07-1.28x, headless ~1.05x
(1.12x in wall mode). `make TSAN=1` builds with ThreadSanitizer.

`--event-trace F` writes every guest-visible device event (Cause IP latches, IRQ entries, frame ends, GPU launch /
busy / ready, UART bytes, MMIO writes, MMIO reads whose value changed) with its instruction count;
`tests/compare_runs.py "A" "B"` runs two builds or modes over all test programs and proj7 and compares UART, event
traces, guest-state stderr and frame dumps byte for byte.

## Tests
`make test` runs `check-hex`, `test-hex`, `test-hw`, `test-quirks`, `test-player` (all with `--sync`) and
`test-threads` (`tests/test_threads.py`: the threaded GPU model, determinism of threaded runs, the player with
`--threads=on`). `make test-hex` runs `tests/test_hex.py`
(loop, uart, lcd, alias, frame, gpcmds/gplaunch with `check_gp.py`, prng, hwprobe). `make test-hw` runs `tests/test_hw.py`:
`tests/hw/*.s` checks UART TX timing, the RX edge and holding register, the PixelFeeder frame latch, the timer and
interrupt entry, the GP_STATE layout, registers at reset and `--legacy-io`, the guest exit register (and `--no-guest-exit`), and makes every guest-bug detector warn,
halt (exit 4) and go quiet, and checks the stdout/stderr split and `--color`. `make test-player` runs `tests/test_player.py` (proj7 player moved in 8 directions + idle via
`--input-script` UART keys and UARX interrupts). `make test-quirks` runs `tests/test_quirks.py`: one tiny program per
quirk for detection, with clean counterparts, each quirk on and off against the standard MIPS result, and the CLI and
environment interface (list syntax incl. `no-NAME`, `--opt VALUE` forms, `--help`). How to run them and their pass conditions are in NOTES.md.

**Test programs are built from assembly.** Every test's machine code is a `.s` file: `tests/*.s` (the programs above),
`tests/hw/*.s` (the hardware tests, macros in `tests/hw/hw.inc`) and `tests/quirk/det_*.s` / `emu_*.s` (the quirk snippets). `make tests` (a dependency of the test targets) assembles
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
- **Right, line 1:** run state (RUN, STOPPED (break / max-insns / error), EXITED (guest code N) or HALTED (quirk)), instructions, the displayed PF_FRAME and the number of PF_FRAME writes, GP launches and injected IRQs.
- **Right, line 2:** when D-scratch holds proj6/proj7 shared memory (magic 0xFEDBEEFn at 0x50000000): graphics paused (flags bit 0, ' ' key), clock paused (flags bit 1, 'd'/'e' keys) and the BCD mm:ss clock.
- **Right, bottom:** recent keys. Window keystrokes are shown when pressed, `--input-script` keys when due, and `--uart-in` and stdin bytes when the guest reads them. The newest key is at the right and older keys scroll out to the left.
- `--screenshot F.png` writes the whole window, strip included, as PNG when the run ends (needs a window; works under `xvfb-run`).

- Frame N starts at `0x10000000 + N<<22`. The visible area is 800×600, rows start 4096 bytes apart, and each pixel is a 32-bit big-endian XRGB word.
- **Window cost** (measured, see NOTES.md "Window cost"; single-threaded, i.e. `--sync`, numbers): the window costs ~18% against `--no-window` (50M
  instructions: 5.6 s vs 4.6 s; with the default `--threads=on` most of it moves to the main thread, NOTES.md "Threads"); the status strip is ~0.4 ms of a ~2.6 ms redraw (~2% of the run). The status text is
  re-read at most 4 times a second.
- A PF_FRAME write (0x80000050) is shown from the next frame end (as on hardware; details in NOTES.md), and the window
  is redrawn at every frame end (also when the frame did not change, since the GPU may have drawn into it). With
  `--frame-clock=emulated` it is also redrawn when nothing drew for 1/60 s of host time; with `--frame-clock=wall` the
  ~60/s frame ends do the drawing and that timer is only a 0.1 s fallback.
- **Status strip, line 2** also shows `frames N R/s emul|wall`: frame ends so far and their rate per wall second
  (re-measured at most 4 times a second).
- Options:
  - `--no-window` runs without a window.
  - `--dump-frame F.ppm` writes the selected frame to F.ppm when the run ends.
  - `--hold` keeps the window open after the run.
  - Closing the window stops emulation.

## Default BIOS (gios)
proj_app* programs are linked against **gios**: `FIXED_ELF ?= ../gios/gios.elf` in `software/proj_app7/Makefile:5`. With bios150v3 loaded instead, proj7 jumps to address 0 because the two BIOS images put different functions at the same addresses.
- By default vvizard loads `SW/gios/gios.inst.bin` raw at 0x40000000, SW being `--sw-dir` or `<exe dir>/../../software`
  (resolved at run time; nothing is compiled in).
- The default BIOS is skipped when any `--load FILE@0x4xxxxxxx` is given. `--bios FILE` overrides the default, and `--no-bios` disables it.
- If the default file is missing: with the default program that's an error (exit 1); with your own `--load`/ELF it's a
  warning and the run continues.

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
