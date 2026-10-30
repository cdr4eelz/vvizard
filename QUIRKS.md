# MIPS150 hardware quirks and bugs

This is the complete list of known places where the MIPS150 CPU (the Verilog in `cs150/hardware/src`) behaves
differently from standard MIPS32 (what Unicorn executes). vvizard can emulate each one, and each can be turned on or
off. The default set is **the current hardware** (cs150 commit fcdda7f). RTL paths below are relative to
`hardware/src`.

## Using the quirks

    ./vvizard --list-quirks              # this list, with defaults and RTL refs, from the binary
    ./vvizard --quirks=LIST ...          # quirks to emulate (default: default)
    ./vvizard --no-quirks ...            # plain MIPS32 (= --quirks=none)
    VVIZARD_QUIRKS=LIST ./vvizard ...    # applied first; --quirks overrides it
    make run HW_QUIRKS=LIST              # Makefile default: HW_QUIRKS ?= default

LIST is comma-separated: `default`, `none`, `all`, `NAME`, `-NAME`, `+NAME`. If it starts with `-NAME` or `+NAME`,
it edits the set so far (the defaults, or `VVIZARD_QUIRKS`). Otherwise it builds a new set. Examples:
- `--quirks=-load-use`: current hardware without the load-use emulation.
- `--quirks=+jalr-rd,+lb-sign`: hardware before fcdda7f.
- `--quirks=none,hilo`: only `hilo`.

The old name `uart-lane` is accepted for `mmio-lane`.

**Detection** is separate from emulation:
- `--halt-on-quirk[=LIST]` stops *before* the instruction that hits a quirk. It prints the PC, the instruction and
  its disassembly, the quirk name, the hardware behaviour and the registers, then exits with status 4.
- `--warn-on-quirk[=LIST]` logs each (quirk, PC) once, tagged `[emulated]` or `[not emulated: standard MIPS]`, and
  keeps running.
- With no LIST, both report the quirks being emulated. `-NAME` removes one from that set, and `all` also reports
  quirks that aren't emulated, such as the fixed `jalr-rd` and `lb-sign`.
- Both print a `[quirk] summary` of hit counts at exit.
- `make run` passes `QUIRKS ?= --halt-on-quirk`.
- If an enabled quirk can't be reproduced at a given site, vvizard prints `[quirk] NAME ... NOT emulated (why)`
  once per PC and uses the standard result.

**Evidence and tests:**
- **hwprobe #N** is check number N (counting from 0, in program order) in `tests/hwprobe.s`. It was run on the RTL
  with the sim-icarus rig (CPU_prog_tb + CoreHarness) and in vvizard (`make test-hex`, which expects all 45 to
  match with the default quirks).
- **test_quirks** is `tests/test_quirks.py` (`make test-quirks`):
  - *det* programs must halt under `--halt-on-quirk=all` with exactly that quirk;
  - *emu* programs must give the hardware result with `--quirks=+NAME` and the MIPS32 result (or trap) with
    `--quirks=-NAME`.

## Summary

| quirk | default | status | confirmed by |
|---|---|---|---|
| [`bal-untaken`](#bal-untaken) | on | current hardware (RTL bug, fix postponed) | hwprobe #21, #44 |
| [`load-use`](#load-use) | on | current hardware (design: no interlock) | hwprobe #23-25 |
| [`unmapped`](#unmapped) | on | current hardware | RTL reading |
| [`unmapped-fetch`](#unmapped-fetch) | on | current hardware | RTL reading |
| [`bios-store`](#bios-store) | on | current hardware | RTL reading |
| [`ddr2`](#ddr2) | on | current hardware; the load half is not emulated | RTL reading |
| [`unaligned`](#unaligned) | on | current hardware | hwprobe #0-4 |
| [`lwlr`](#lwlr) | on | current hardware | hwprobe #5-8 |
| [`overflow`](#overflow) | on | current hardware | hwprobe #12-14 |
| [`hilo`](#hilo) | on | current hardware | hwprobe #9-11 |
| [`syscall`](#syscall) | on | current hardware | hwprobe #15 |
| [`reserved`](#reserved) | on | current hardware | hwprobe #16-22, #26-27, #33-40 |
| [`cop0`](#cop0) | on | current hardware | hwprobe #28-32 |
| [`mmio-lane`](#mmio-lane) | on | current hardware | RTL reading |
| [`jalr-rd`](#jalr-rd) | **off** | fixed in cs150 fcdda7f | hwprobe #43 (fixed behaviour) |
| [`lb-sign`](#lb-sign) | **off** | fixed in cs150 fcdda7f | hwprobe #41-42 (fixed behaviour) |

**Default rule:** a quirk is on if the current hardware still behaves that way. The two bugs fixed in fcdda7f are
off but kept so that older bitstreams can be reproduced. `bal-untaken` is a real RTL bug, but the user rated it
obscure and postponed the fix, so it stays on until the RTL changes.

## Quirks

### bal-untaken
- **Default:** on. This is a current-hardware bug; the fix is postponed.
- **Hardware:** an untaken `bltzal`/`bgezal` doesn't link, so `$31` keeps its old value. (The same holds for the
  REGIMM `al` variants handled by `reserved`, such as `bltzall`.)
- **MIPS32:** `$31 = PC+8` whether or not the branch is taken.
- **RTL:** `cpu/StageDX.v:105`. The link write is gated by the branch-taken condition.
- **Confirmed:** hwprobe #44 (untaken `bltzal`, `$ra` stays 0) and #21 (untaken `bltzall`).
- **Tests:** test_quirks det `bal_untaken` and emu `bal_untaken` (`ra=0` with the quirk on, `ra=pc+8` with it off).
  The bal-untaken XFAIL in the cs150 Icarus suite tracks the RTL fix.

### load-use
- **Default:** on. This is the pipeline design.
- **Hardware:** there is no load interlock and no forwarding from a load. The instruction right after a load reads
  the OLD value of rt. If that instruction writes rt itself, its result wins over the load.
- **MIPS32 (Unicorn):** the next instruction sees the loaded value. (MIPS I had a load delay slot, but its contents
  were undefined.)
- **RTL:** `cpu/StageMW.v:75-76` (`WBK_CanFWD=0` for loads) and `cpu/CPUMIPS.v:131`.
- **Confirmed:** hwprobe #23-24 (the next instruction sees 0x1111 and later reads see the loaded value) and #25 (an
  `addiu` in the slot wins).
- **Emulation:** the next instruction runs with the old value, then the loaded value is restored unless that
  instruction wrote rt. A load in a branch delay slot is checked against the branch target, as in the pipeline.
- **Tests:** test_quirks det `load_use` / `load_use_ok` and emu `load_use`.

### unmapped
- **Default:** on.
- **Hardware:** a load or store to an undecoded address nibble (0x0, 0x7, 0x9-0xB, 0xD-0xF) doesn't raise an
  exception. The load reads 0 and the store is dropped.
- **MIPS32/Unicorn:** a TLB/bus fault (`UC_ERR_EXCEPTION`).
- **RTL:** `cpu/MemBank.v:69-83` (no write enable), `cpu/MemBank.v:132` (`MUX_DMEM` default = `DEAD_DMEM`) and
  `cpu/MemBank.v:8` (`DEAD_DMEM = 0`).
- **Confirmed:** RTL reading. The address-0 accesses in hwprobe #39-40 go through this path.
  [CoreHarness](#coreharness-deadbeef) returns 0xDEADBEEF instead of 0 there.
- **Emulation:** `tlb_fill` maps undecoded data pages to a zero "VOID" page, which is re-zeroed after every store.
- **Tests:** test_quirks det `unm_load`, `unm_store` and emu `unm_load`.

### unmapped-fetch
- **Default:** on.
- **Hardware:** a fetch from an undecoded nibble, or from MMIO (0x8), reads 0, so the CPU runs nops.
- **MIPS32/Unicorn:** a fetch fault.
- **RTL:** `cpu/MemBank.v:104-113` (`MUX_IMEM`: 0x8 "Can't execute IO!" and the default case are `DEAD_IMEM`) and
  `cpu/MemBank.v:8` (`DEAD_IMEM = 0`).
- **Confirmed:** RTL reading.
- **Tests:** test_quirks det `unm_fetch`, `mmio_fetch` and emu `unm_fetch` (the nops run until max-insns; with
  the quirk off the run faults).

### bios-store
- **Default:** on.
- **Hardware:** a store into the BIOS (0x4xxxxxxx) is dropped, because the BIOS is data-read-only.
- **MIPS32/Unicorn:** the memory is RAM, so the store sticks.
- **RTL:** `cpu/MemBank.v:75`.
- **Confirmed:** RTL reading.
- **Emulation:** the word is saved before the store and written back after it. The `[warn] data write to BIOS`
  log line says whether the write was dropped or kept.
- **Tests:** test_quirks det `bios_store` and emu `bios_store`.

### ddr2
- **Default:** on.
- **Hardware:** 0x2xxxxxxx is the I-cache path to DDR. A store there from code running outside the BIOS is dropped.
  A load returns whatever the D-cache data output holds (stale data).
- **MIPS32/vvizard:** 0x2 is an alias of DDR, so the access goes through.
- **RTL:** `cpu/MemBank.v:80` (store enable only when PC is in the BIOS) and `cpu/MemBank.v:130` (load mux).
- **Status:** the store half is emulated. The load half is **not emulated** (it is reported once per PC); vvizard
  returns the DDR data.
- **Confirmed:** RTL reading.
- **Tests:** test_quirks det `ddr2` and emu `ddr2` (runs from I-scratch at 0x60000000).

### unaligned
- **Default:** on.
- **Hardware:** an unaligned `lw`/`lh`/`lhu` writes 0 and an unaligned `sw`/`sh` is dropped, with no AdEL/AdES.
  On MMIO, the access has byte mask 0, which MemMapIO treats as a read, so it can pop UART RX.
- **MIPS32/Unicorn:** an AdEL/AdES exception.
- **RTL:** `innerds/ByteAccess4.v:36,69` and `cpu/MemMapIO.v:65-66,97`.
- **Confirmed:** hwprobe #0-2 (`lw`/`lh`/`lhu` give 0) and #3-4 (unaligned `sw`/`sh` leave memory unchanged).
- **Emulation:** the base register is redirected to the VOID page for that one instruction, then restored. On MMIO,
  vvizard also performs the read's side effect (an RX pop).
- **Limit:** an access with base register `$zero` is not emulated.
- **Tests:** test_quirks det `unaligned` and emu `unaligned`.

### lwlr
- **Default:** on.
- **Hardware:** `lwl`/`lwr` write 0 to rt and `swl`/`swr` store nothing. With MemShift=1, ByteAccess4 falls into
  its default case. On MMIO the access counts as a read.
- **MIPS32:** a partial-word merge.
- **RTL:** `innerds/ByteAccess4.v:36,69`.
- **Confirmed:** hwprobe #5-6 (`lwl`/`lwr` give 0) and #7-8 (memory unchanged after `swl`/`swr`).
- **Tests:** test_quirks det `lwl` and emu `lwlr` (with the quirk off, `lwl` merges 0x22334477).

### overflow
- **Default:** on.
- **Hardware:** signed overflow in `add`/`addi`/`sub` wraps, with no Integer Overflow trap.
- **MIPS32:** an Ov exception, and rd is not written.
- **RTL:** `innerds/ALUdec.v:33-36,61`.
- **Confirmed:** hwprobe #12-14.
- **Tests:** test_quirks det `overflow` / `add_ok` and emu `overflow`.

### hilo
- **Default:** on.
- **Hardware:** there is no HI/LO. `mult`/`multu`/`div`/`divu`/`mthi`/`mtlo` do nothing, and `mfhi`/`mflo`
  write 0 (`ALU_XXX` gives 0).
- **MIPS32:** a real multiply and divide unit.
- **RTL:** `innerds/ALUdec.v:44-47`.
- **Confirmed:** hwprobe #9-11.
- **Tests:** test_quirks det `hilo` and emu `hilo` (3*5 gives 0 with the quirk on and 15 with it off).

### syscall
- **Default:** on.
- **Hardware:** `syscall` is a nop and raises no exception.
- **MIPS32:** a Sys exception.
- **RTL:** `innerds/ALUdec.v:44`.
- **Confirmed:** hwprobe #15.
- **Note:** `break` is also unimplemented in hardware, but vvizard keeps it as its clean stop.
- **Tests:** test_quirks det `syscall` and emu `syscall`.

### reserved
- **Default:** on.
- **Hardware:** instructions the MIPS150 doesn't decode behave as follows:
  - an unknown SPECIAL funct (`movz`, `movn`, `sync`, `tge`/`teq`/..., `movci`) writes 0 to rd;
  - `rotr` and `rotrv` act as `srl` and `srlv`;
  - branch-likely (`beql`, `bnel`, `blezl`, `bgtzl`) is a nop;
  - REGIMM with rt not in {0, 1, 16, 17} acts as `bltz`/`bgez` selected by rt[0], linking if rt[4]. It is an
    ordinary branch with a delay slot, so `bltzl`/`bgezl` are not "likely", and trap-immediates such as `tgei`
    **branch**;
  - SPECIAL2/3 (`mul`, `clz`, ...), COP1-3, `ll`, `sc`, `lwc*`, `swc*`, `pref`, and COP0 operations other than
    `mfc0`/`mtc0` (`eret`, `di`, `ei`, `wait`, TLB operations) are nops;
  - opcodes 0x27, 0x2C, 0x2D and 0x2F decode as `lw`/`sb`/`sh`/`sw` with no ALU operation, so they access address
    0 (load 0, store dropped).
- **MIPS32:** RI or the instruction's real semantics.
- **RTL:** `innerds/InstructionControl.v:33,58-59,69,86,125-127` and `innerds/ALUdec.v:22,75`.
- **Confirmed by hwprobe:**
  - #16-17: `movz`/`movn`
  - #18-19: `beql`/`bnel`
  - #20: untaken `bltzl` runs its delay slot
  - #21: `bltzall`
  - #22: taken `tgei` branches with a delay slot
  - #26-27: `rotr`/`rotrv`
  - #33: `eret`
  - #34: `clz`
  - #35: `mul`
  - #36: `ll`
  - #37-38: `sc`
  - #39: op 0x27
  - #40: op 0x2F
- **Emulation:** nops are skipped. A REGIMM branch that hardware takes but Unicorn would trap is emulated as a
  branch with a delay slot.
- **Limit:** a nop-class instruction inside a branch delay slot is **not emulated** (it is reported).
- **Tests:** test_quirks det `reserved` (`movz`) and emu `movz`, `tgei`.

### cop0
- **Default:** on.
- **Hardware:** COP0150 has only these registers:
  - Count (9): +1 per cycle;
  - Compare (11): resets to 0xFFFF; a write clears the timer IP bit, Cause[15];
  - Status (12): resets to 0, all 32 bits read/write, no hidden masking;
  - Cause (13): IP bits 15:10 can only be cleared;
  - EPC (14): written only on interrupt entry, and `mtc0` to it is ignored.

  Every other register reads 0, and the `sel` field is ignored.
- **MIPS32/Unicorn:** the full CP0 register set, with Unicorn's reset Status 0x20400004 and its masking. Count
  doesn't run.
- **RTL:** `cpu/COP0150.v:24-28,35,61,70-75,80-84`.
- **Confirmed:** hwprobe #28 (`mtc0 EPC` ignored), #29 (`$15` reads 0), #30 (Compare 0xFFFF), #31 (Status
  0xFFFF0000 reads back) and #32 (`sel=1` reads Status).
- **Emulation:** a shadow register model in main.c (`vv_cop0_read`/`vv_cop0_write`). `--irq` uses the shadow
  Status for IE/IM. Count counts instructions, not cycles.
- **Tests:** test_quirks det `cop0` and emu `cop0`. `make test-player` exercises the ISR path (Cause/EPC/Status)
  with this model.

### mmio-lane
- **Default:** on.
- **Hardware:** an `sb`/`sh` to an MMIO register delivers ByteAccess4's lane-placed word: the data in its
  big-endian byte lane, zeros elsewhere. UART TX sends `dina[7:0]`, so an `sb` to UART TX at +0x8 sends 0x00 and
  only +0xB (or `sw`) sends the byte.
- **vvizard without the quirk:** the MMIO register sees the raw byte value.
- **RTL:** `innerds/ByteAccess4.v` (WordMasked) and `cpu/MemMapIO.v:99`.
- **Confirmed:** RTL reading. The cs150 PRNG Icarus check confirms the same lane placement for the PRNG reseed,
  which vvizard applies whether or not the quirk is on.
- **Tests:** test_quirks det `mmio_lane` / `uart_ok` and emu `mmio_lane` (stdout `\0` with the quirk on, `A` with it
  off). test_hex `uart` (`sb` at +0xB) and `prng` pass with the quirk on and off.

### jalr-rd
- **Default:** off. Fixed in cs150 commit **fcdda7f**.
- **Old hardware:** `jalr rd, rs` always linked to `$31`, whatever rd was.
- **MIPS32 and current hardware:** link to rd.
- **RTL (old):** `innerds/InstructionControl.v:115`.
- **Confirmed:** hwprobe #43 shows the fixed behaviour. The old behaviour came from RTL reading and the cs150
  StageDX/jalr testbenches that caught the bug.
- **Tests:** test_quirks det `jalr_rd` / `jalr_ok` and emu `jalr_rd`. Bare `--halt-on-quirk` must *not* report it
  (it is off by default).

### lb-sign
- **Default:** off. Fixed in cs150 commit **fcdda7f**.
- **Old hardware:** `lb`/`lh` zero-extended like `lbu`/`lhu`.
- **MIPS32 and current hardware:** sign extension.
- **RTL (old):** `innerds/ByteAccess4.v` (ValExtract had no IsSigned).
- **Confirmed:** hwprobe #41-42 show the fixed behaviour. The old behaviour came from the cs150 ByteAccess4
  testbench.
- **Tests:** test_quirks det `lb_neg`, `lh_neg`, `lb_pos` and emu `lb_sign`.

## Differences that are not emulated
These are found by RTL reading. vvizard keeps its standard or simplified behaviour.
- **Stale load via 0x2** (`cpu/MemBank.v:130`): reported under `ddr2`, but not reproduced.
- **Split write-through caches:** the I-cache doesn't see D-path writes, so self-modifying code or freshly copied
  code can run stale instructions on hardware. vvizard has one coherent memory.
- **CNT_CYCLE** (`cpu/MemMapIO.v`) counts every clock, including stalls. vvizard returns the instruction count for
  both CNT_CYCLE and CNT_INST, and COP0 Count also counts instructions.
- **Timer and RTC interrupts** (`cpu/COP0150.v:35`: Count == Compare; and Count == 0xFFFFFFFF) are never raised.
  `--irq` injects only UART RX. The GPU-done interrupt (IB_GPU=12) is not raised either.
- **Interrupt deferral:** `cpu/CPUMIPS.v:114` defers an interrupt on couldBranch, mtc0, WAS_Branch and WAS_ISR.
  vvizard defers only in branch delay slots.
- **Mask-0 MMIO store** (`cpu/MemMapIO.v:65-66`): `isRead = ena && ~|wea`, so any MMIO access with an empty byte
  mask is a read and pops RX_DATA. vvizard does this only for the redirected `unaligned`/`lwlr` cases.
- **RX_DATA read when not valid** returns junk (`cpu/MemMapIO.v:101`), and undefined MMIO offsets read `32'dx`
  (`cpu/MemMapIO.v:166`). vvizard returns 0 and logs a MISS-READ.
- **J-type target** uses the jump's own PC[31:28] (`cpu/StageDX.v:96`), not the delay slot's. This only matters
  when the jump is the last word of a 256 MiB segment.
- **`jr` to a misaligned target:** hardware ignores address[1:0], while Unicorn raises AdEL.
- **Unicorn CP0 state:** with `cop0` off (or for bits the model doesn't cover), Unicorn's Status reset value
  0x20400004 and its write masking apply.
- **Delay-slot limits:** a nop-class `reserved` instruction or `syscall` in a delay slot, and an
  `unaligned`/`lwlr` access with base `$zero`, are reported as NOT emulated.
- **MMIO fetch perms:** under `unmapped-fetch`, a fetch from MMIO gets an execute-only VOID mapping. A data access
  to the same page right after it is not separately verified.

## CoreHarness DEADBEEF
The cs150 sim-icarus `CoreHarness.v` (used by `CPU_prog_tb`) returns **0xDEADBEEF** for a load from an unmapped
address. The real `MemBank.v` returns `DEAD_DMEM = 0` (`cpu/MemBank.v:8,132`). So hwprobe **#39** (op 0x27 loads
from address 0) reads 0xDEADBEEF on the Icarus rig and fails there by design. All other 44 checks match the RTL.
vvizard follows MemBank and returns 0, so #39 passes in vvizard. Any rig test that relies on unmapped loads reading
0 needs MemBank, or a CoreHarness with its dead value set to 0.

## Reproducing the probe
- **vvizard:** `make test-hex`, or by hand:
  ```
  vvizard --no-bios --no-random-ddr --load hwprobe.bin@0x60000000 --entry 0x60000000 --expect s5=0 --expect s3=45
  ```
  `$s5` counts mismatches, `$s4` holds the first failing index + 1, and `$s3` counts the checks.
- **RTL:**
  - Assemble `tests/hwprobe.s` with `-I hardware/sim-icarus/tb/data` (for `check.inc`); the build line is in the
    file header.
  - Convert the binary to `probe.hex` and compile `CPU_prog_tb` + `CoreHarness` with `-DMAIN_HEX=\"probe.hex\"`.
  - The testbench prints each CHECK by its index.
