# VVIZARD design notes

Background and probe findings moved out of README.md. Nothing here is needed to build or run vvizard.

## Memory map: hardware references
- The hardware decodes only `ADDR[31:28]` (MemBank.v:51,70; StageMW.v:6-21).
- DDR: 0x1 = D$ path, 0x2 = I$ path, 0x3 = both; address `{4'h0,addr[27:0]}` (MemBank.v:141,150).
- BIOS: indexed by `addr[13:2]` (MemBank.v:160); data-read-only in hardware.
- MMIO: MemMapIO.v gets `addra = DMEM_ADDR[13:2]` (MemBank.v:211), so the map repeats every 16 KiB. Since the
  cs150 `lcd-text` branch (commit c2e8221) the registers are qualified with `io_base = addra[11:6] == 0` and answer only at
  0x80000000-0x800000FC (they used to repeat every 0x100 on `addra[5:0]`). `LCDText2x16_MemPort` takes
  `addra[11:5] == 2` (0x80000100-0x8000017C): `we = ena && |wea`, `waddr = addra[4:0]`, `wdata = dina[7:0]`, reads 0.
  Elsewhere reads return `32'dx` (BADNESS=0); vvizard returns 0.
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
MMIO uses `uc_mmio_map` over the 16 KiB map. Its callbacks take `off & 0x3FFC` (`VV_MMIO_DECODE`): offsets below 0x100
reach the registers, 0x100-0x17C the LCD buffer (`src/lcd.c`), anything else is a miss.
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
Programs are built from `tests/*.s` into `build/tests/*.bin` (`make tests`; README "Tests"); the committed
`tests/*.hex` are the no-toolchain fallback (convert by hand with `perl -ne 'print pack("H*",join("",split))'`).
When the build moved into the Makefile (commit "Build test programs from assembly"), loop, uart, alias, frame,
gplaunch and the 41 quirk snippets (previously hex only) were disassembled into `.s` and re-assembled byte-identical;
gpcmds became `.word` data; hwprobe, lcd and prng re-assembled identical except that the old hand-made hex carried
the assembler's 16-byte section padding at the end (2, 1 and 1 zero words after the last instruction), which the
Makefile build leaves out (`--no-pad-sections`).
- `loop` (at 0x40000000): passes if t0=30 and t2=0x11e.
- `uart`: writes "H" to 0x80000008, "i" to 0x80000108 and "\n" as a byte store to 0x8000000B. 0x80000108 used to be a UART alias; now it is LCD char 2. Passes if stdout is exactly `H\n`, LCD char 2 is `i` (`--dump-lcd`) and t2 (the TX_READY read) is 1.
- `lcd` (source `tests/lcd.s`, at 0x40000000, run with the default quirks and with `--quirks=-mmio-lane`): `sw` to chars 0, 1, 15, 16 and 31 (one with junk in bits 31:8), `sb` at +3 and +0, `sh` at +2 and +0, a store through the 16 KiB alias 0x80004100, a store to 0x80000108 (old UART alias), stores to 0x80000180 and 0x80000208 (dropped), and `\n` to 0x80004008 (16 KiB UART alias). Reads 0x80000100/104 (0), TX_READY at 0x80000000 and 0x80004000 (1), 0x80000100 (0, not TX_READY) and 0x80000184 (0). Passes if stdout is exactly `\n`, the registers match, and `--dump-lcd` shows line 1 `QiU`, 0x00, `X`, 0x00, 9 spaces, `L` (with `mmio-lane`; `QiUXXX` without) and line 2 `a`, 14 spaces, `z`.
- `alias` (runs at 0xC0000180): stores to 0x12003000 and reads back through 0x32003000, before and after `mtc0 $0,Status`.
- `frame`: fills x 100..131, y 50..59 of frame 1 with 0x00FF8000, writes 1 to PF_FRAME, then `break`.
- `prng` (source `tests/prng.s`, at 0x40000000, `--max-insns 300000 --no-random-ddr`): reads the PRNG unseeded, stores 0xDEADBEEF and reads at +1 (stale), +2, +3, +5 cycles, reseeds with 0, `sb` at +3 and at +0, `lbu`, a store to 0x80000180 (no longer a PRNG alias, so it must not reseed), a CNT_RESET store (must not disturb the PRNG), then reads after a 196608-instruction loop (exercises the jump-ahead). Passes with
  `--expect a0=0x56709234 --expect a1=0x2b38491a --expect s0=0xe28b8493 --expect s1=0xdeadbeef --expect s2=0xef76df74 --expect s3=0x3bddb7dd --expect s4=0x1 --expect s5=0xab --expect s6=0xab000000 --expect s7=0x0 --expect t3=0x0ab00000 --expect t5=0x9484c744`.
  The first 10 values were checked against the RTL: an Icarus testbench drove cpu/MemMapIO.v with the same MMIO access in the same cycle (cycle = instruction number, state(0) = reset value) and read the same values. t3 and t5 changed with the new decode (the 0x80000180 store no longer reseeds): they are step^4 and step^196614 of the 0xAB000000 seed, checked with a separate Python LFSR model, not yet with the RTL. The RTL side of "0x80000280 is not the PRNG" is the `memmapio_prng` Icarus test on the lcd-text branch.
- `gpcmds` + `gplaunch`: load gpcmds at 0x12003000 and gplaunch at 0x40000000, run with `--dump-frame`, check with `tests/check_gp.py`.
- `hwprobe` (source `tests/hwprobe.s`, at 0x60000000, `--no-bios --no-random-ddr`): passes with `--expect s5=0 --expect s3=45` (default quirks).
- `make test-hex` runs all of the above (`tests/test_hex.py`); `make test` = test-hex + test-quirks + test-player.
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

## Hardware quirks: emulation and detection (`src/quirk.c`, `src/quirk.h`)
The quirk list, defaults, evidence and test coverage are in [QUIRKS.md](QUIRKS.md). This section covers the implementation.
- **Table.** `VV_QUIRK_TABLE` in quirk.h lists each quirk with its name, its default, and the hardware behaviour
  with RTL file:line. `vv_quirk_emul` is the set being emulated and `vv_quirk_detect` is the set
  `--halt-on-quirk`/`--warn-on-quirk` report on. Order of resolution: defaults, then `VVIZARD_QUIRKS`, then
  `--quirks`/`--no-quirks`. The detection LIST is relative to the emulated set.
- **Hooks.** There are no new Unicorn hooks. The existing `UC_HOOK_CODE` calls three functions in turn:
  - `vv_quirk_post()` finishes the PREVIOUS instruction. It restores or sets registers, restores HI/LO, puts back
    a RAM word overwritten by a dropped store, re-zeroes the VOID page, and runs the `lb-sign` check. It also takes
    a pending emulated branch.
  - `irq_step()`.
  - `vv_quirk_pre()` decodes the instruction about to run, reports quirks and sets up the emulation.

  If `irq_step()` redirected to the ISR, the pending state is dropped instead. The hooks are active whenever any
  quirk is emulated or detected.
- **Emulation techniques** (pre/post pairs):
  - **Result fix-up:** `hilo`, `overflow` (operands are zeroed while the instruction runs, so it can't trap, then
    restored, and the wrapped result is written), `rotr`, the `movz` class, `cop0` mfc0, `jalr-rd` and `lb-sign`.
  - **Skip** (PC := pc+4 from the code hook) for instructions the hardware treats as nops. This isn't possible in a
    delay slot, so those are reported as "NOT emulated".
  - **Base-register redirect.** For an unaligned access or the `lwl` family, rs is set so the effective address is
    `VV_VOID_VA` (0xF0000800, aligned), and rs is restored afterwards. On MMIO, `vv_mmio_side_read()` reproduces
    the read the hardware does (an RX_DATA pop).
  - **VOID page.** A 4 KiB zero page at PA 0xF0000000. `tlb_fill` maps undecoded VAs to it (data accesses under
    `unmapped`; fetches, including MMIO fetches, under `unmapped-fetch`; the VOID_VA page under
    `unaligned`/`lwlr`). It is re-zeroed after every store into it.
  - **Dropped stores** to BIOS, or to 0x2 from outside the BIOS: the 4 bytes at the word are saved in pre and
    written back in post.
  - **Load-use.** The instruction after a load gets the OLD rt (rt as it was before the load) while it runs, then
    the loaded value is restored, unless the instruction writes rt itself. Its result then wins, as in the
    pipeline. The register swap happens first, so address and overflow calculations see the old value too.
  - **Reserved REGIMM branches the hardware takes** (`tgei` when rs<0, for example): skip, link if rt[4], and
    after the delay slot runs, set PC to the target (`vv_quirk_post` returns 1 and the hook returns, with no
    instruction counted).
  - **`mmio-lane`** lives in `mmio_write()`: the sub-word data is lane-placed before the register switch. The
    PRNG reseed placement is unchanged.
  - **COP0150 model** (`vv_cop0_read`/`vv_cop0_write` in main.c): shadow Status, Compare, Count, Cause and EPC.
    `irq_step` uses the shadow Status for IE/IM when `cop0` is on, and keeps its old ISR-only mfc0/mtc0 shadowing
    when `cop0` is off. Count is one tick per instruction (no stall model).
- **Not emulated:** see [QUIRKS.md](QUIRKS.md#differences-that-are-not-emulated) for the hardware differences vvizard does not reproduce.
- **Halting:** `uc_emu_stop()` from the code hook stops before the flagged instruction runs. Exit status 4.
- **Speed** (GIOS+proj7, headless, 20M instructions, -O0): `--no-quirks` 4.4 s, default 5.8 s, default plus
  `--halt-on-quirk` 5.8 s.
- **Probe:** `tests/hwprobe.s` (45 checks, verified on the RTL; see [QUIRKS.md](QUIRKS.md#coreharness-deadbeef) for the #39 CoreHarness note) passes with the defaults (`make test-hex`). With `--no-quirks` it stops at the first unaligned `lw` (AdEL).

### Findings on current GIOS / proj7 / isr7 (Oct 28 2026)
- Headless, `--warn-on-quirk`: 20M insns idle; 60M insns with ~1180 scripted keys (moves, long runs off every
  screen edge, space, d/e, junk keys); 70M insns with ~25 GIOS commands (lw/lbu/lhu/sw/sb/sh/dump/gs/result/
  color/fill/line/rect/elip/pf/sf/back/copy/unknown) then proj7 + keys; `test_player` with `--halt-on-quirk`.
- (Run with the pre-fcdda7f hardware; lb/lh now sign-extend, so `lb-sign` is off by default.) Only trip in program code was: **`lb-sign` in isr7.s `_uarx_player_move`** at 0xC0000404 (`lh SMO_player_x`,
  isr7.s:323) and 0xC0000414 (`lh SMO_player_y`, isr7.s:328), when the player x/y goes negative (moved past the
  left/top edge). Benign for the ISR itself: it only does `addu` + `sh`, and the low 16 bits are identical.
- User-driven only: GIOS `lw 70000000` -> `unmapped` at 0x40000200 (gios.c:71 BC_LOAD); `lw 50000001` ->
  `unaligned` at the same PC; `sw X 40000100` -> `bios-store` at 0x40000284 (gios.c:89 BC_STORE).
- Static scan of the dumps: every `jalr` in gios/proj7 has rd=$31 (0x40001f04 `jalr zero,zero` and the
  mfhi/mthi/mflo/addi hits at 0x40001fdc+ are data words); gios's 12 `lb` read string bytes, so they trip only on
  bytes >= 0x80; no add/sub, mult/div, syscall, lwl/lwr in code.
