# Speed up `--cpu=fast2`

> **Status: plan only, not implemented.** Drafted by a Grok Build plan-mode session against the `feat/custom-mips-backend` tree; fast2 itself is not on `main`. Saved here for later.

Plan only. No code changes in this step. `--cpu=fast` and the Unicorn hook stay as they are. The two interpreters stay separate.

Measured baseline (UBU24, proj7, `--sync --no-window`, 50 million instructions): `--cpu=fast` about 48.9 M insn/s, `--cpu=fast2` about 25.8 M insn/s, `--cpu=unicorn` about 21.2 M insn/s. fast2 already matches Unicorn in `make test-fast2` and is still about 1.9× behind fast. Nothing below promises a new rate. Rank the work, then measure each cut on that same command.

Default build for the measurement is `make PROFILE=0 FIDELITY=l0` (`-O3`, no profiler, no RTL stall model). A `PROFILE=1` binary is for counts only.

## What still runs when detection is off

Detection and emulation are different masks (`src/quirk.h`). `vv_quirk_on(id)` is the emulation bit. `vv_quirk_detect` is the report bit. `vv_quirk_active()` is the OR of the two. `vv_quirk_hit` (`src/quirk.c` `vv_quirk_hit`) always increments `qhits[cat]`, then returns without printing when that category's detect bit is clear.

`src/main.c` sets the masks before the run (around the `qdet_on` / `qbugs` block and the assignment to `vv_quirk_detect`):

- No `--halt-on-quirk`, `--warn-on-quirk`, or `--no-halt-on-quirk`: `vv_quirk_detect` is only the device guest-bug bits that need no instruction decode: `tx-busy`, `rx-overrun`, `irq-storm`, `gp-busy` (`VV_Q_NODECODE_DET` intersected with `vv_quirk_bugs()`). `unmapped-fetch` is in `VV_Q_NODECODE_DET` but its bug column is 0, so it is not in this default mask.
- `--no-halt-on-quirk` is not the same state. It sets `qbugs` and warns on every guest-bug detector, including `undef-reg` and `dcache-stale`, and it does not halt. It does not turn on `load-use` detection. `load-use` is an emulated quirk, not a bug.
- `--halt-on-quirk` / `--warn-on-quirk` turn detection on for the emulated set (unless the list says otherwise) plus every guest-bug detector. Halt returns from `vv_quirk_hit` before the instruction completes.

On the default (detect mostly off) fast2 run, these still execute:

| Work | On by default? | Where |
|---|---|---|
| Load-use **emulation** (next instruction reads the old `rt`) | Yes. `q_lu = vv_quirk_on(LOAD_USE)` in `vv_fast2_init` | `lu_make`, `prep`, `RR` |
| Load-use **report** | No. `prep` still calls `vv_quirk_hit`, which only bumps `qhits` | `prep` |
| `undef-reg` mask maintenance | The detector is off, but `prep` still runs `vv_value_reads` / `vv_writes` on every instruction and updates `vv_reg_defined` | `prep` |
| D-cache stale **emulation** | Yes (`q_dc`). Every aligned DDR load/store still calls `vv_dc_load` / `vv_dc_store` | `do_mem` |
| D-cache stale **report** | No, unless the cached word differs and something else set the detect bit | `do_mem` |
| Device detectors | Yes, inside `hw.c` / the UART and GPU paths, not inside the interpreter | not `exec.c` |
| IRQ sample | Yes. `opt_irq` defaults to 1, and the quiet span still calls `vv_irq_step` | `span` |

`vv_quirk_needs_decode()` is what lets `--cpu=fast` skip `vv_quirk_pre` (`src/cpu/fast.c`, the `qdet` / `qlite` gates in `vv_fast_run`). fast2 does not consult that flag. Its interpreter always does its own prep.

## 1. What is costing time

fast2 is already a basic-block interpreter (`get_bb` / `span` in `src/cpu/fast2/exec.c`). The quiet span skips `vv_hw_tick` between device events. fast (`vv_fast_run` in `src/cpu/fast.c`) ticks every instruction and is still faster, so the gap is the body of each fast2 instruction, plus a few out-of-line calls fast does not make.

Hot path for a straight-line block, per instruction (`span`, the `for` loop that starts after the boot instruction):

1. `needs_tick` on memory ops calls `vv_canon_inl` after it has already returned for nibble `0x8`. No alias in `VV_ALIAS_TABLE` retargets another nibble at MMIO (`src/memmap.h`: only `0x2` and `0x3` fold onto DDR). The canon call does not change the result.
2. `vv_fast2_fetch_aborts`. With `unmapped-fetch` emulation on (the default), it returns at the first `if`. Cheap.
3. `vv_svc_quiet` → `count_insn` in `src/main.c`. Real work (the insn counter, `--max-insns`, `note_ddr_exec` for PCs in `0x1`–`0x3`). Fast does the same counter inline in `vv_fast_run`. This call crosses translation units. There is no LTO in the Makefile.
4. `vv_hw_set_now` (`src/hw.c`) on every quiet instruction. One store, out of line. Fences (`mfc0` / `mtc0` / `eret` / `syscall` / `break` / `OP_FETCHX`) and MMIO already end the span in `needs_tick` / `is_fence`, so a quiet ALU or RAM op does not read the cycle clock.
5. `vv_irq_step` (`src/cpu/irq.c`) whenever `vv_irqs_on()` is true, which is the default. With nothing in Cause and no ISR, the function shuffles `last_pc` / `last2_pc` / `last3_pc` and returns (`vv_hw_irq_possible` is inline; the call is not). Fast's `irq_step_fast` is `static` in `fast.c` and inlines that early-out into the run loop.
6. `exec_dec`.

`exec_dec` then, for every real instruction:

- Calls `prep` (`exec.c`). `prep` is `static` and will inline, but it re-decodes the raw word on every instruction:
  - `vv_value_reads(d->insn)` and `vv_writes(d->insn)` (`src/cpu/mips_decode.h`). Both switch on the opcode. `Dec` already holds `op`, `rs`, `rt`, `rd`, `alu`, `flags`.
  - If `lu_on` (set by every non-zero-`rt` load while load-use is emulated or detected), `vv_reads(d->insn)` again.
  - On an actual use, `vv_quirk_hit(LOAD_USE, ...)` and the caller evaluates `vv_gpr_name` first. With detection off the hit only increments `qhits`.
- Dispatches a coarse `op` into `do_alu` / `do_imm` / `do_mem`.
- Calls `vv_quirk_set_last_pc` (`src/quirk.c`, one store, other file). That PC is only printed by `vv_quirk_fetch`. Default detection does not include `unmapped-fetch`.

`RR` (`exec.c`) is already the cheap part: a flag check, then `gpr[r]`. It is not the analogue of `prep`.

`do_alu` / `do_imm` (`exec.c`):

- Every ALU op calls `alu()`, a second switch, including `addu` / `addiu` / `or` / `and`.
- `add` / `sub` / `addi` always run `__builtin_*_overflow`, then wrap if `q_ov` is set. `addu` / `addiu` do not. proj7 is mostly the unsigned forms; confirm with the profile before spending time here.
- The shift and non-shift arms at the bottom of `do_alu` both call `alu()` the same way.

`do_mem` (`exec.c`) is one large function. An aligned `lw`/`sw` to DDR still walks the flag tests, then:

- `vv_dc_load` or `vv_dc_store` (`src/dcache.c`) on every aligned access whose canonical nibble is DDR and whose raw nibble is `0x1` or `0x3`. This is required while `q_dc` is on, which is the default. Fast does the same call from `mem_load` / `mem_store`.
- `load_ram` copies `size` bytes in a loop. `size` is `d->alu`, so a word load does not become `vv_ram_be32`. Fast's `OP_LW` case passes a constant size into `mem_load`, which is why that loop can unroll. fast2's handler cannot.

`decode.c` `vv_decode_at` runs when a block is built, not per retired instruction, as long as the span stays inside the block. RTL is the exception: `vv_rtl_run` calls `vv_decode_at` again from `dc_stall`, then `vv_fast2_step` decodes once more (`src/cpu/rtl/rtl.c`). That cost is the RTL runner, not the 25.8 M/s fast2 number. The default binary does not compile `rtl.c`.

`--cpu=fast` for comparison (`src/cpu/fast.c`):

- `decode_one` picks a concrete opcode (`OP_ADDIU`, `OP_LW`, `OP_SW`, …).
- `exec_one` reads both registers once, then a direct `rs + rt` or a dedicated `mem_load`.
- Load-use is `gr()`: if `load_rt` matches, return `load_old`. No per-instruction bitmask. `vv_quirk_pre_fast` runs only when `qdet` is set.
- It is not a drop-in replacement. Unknown opcodes become `OP_SLOW` and stop, any store flushes the whole block cache, and it ticks every instruction. Borrow the shape of the hot cases only.

Block lookup (`get_bb`, 16-slot probe) is once per span, not once per instruction, while `vv_svc_quiet_budget` is non-zero. Budget is 0 when an IRQ can be taken (`opt_irq && vv_hw_irq_possible() && Status.IE`). proj7's timer is sparse (NOTES: Compare every 50M cycles). Most instructions should be inside `span`. If a profile shows `get_bb` per instruction, IE was on with Cause latched and the span never ran; fix that separately from the interpreter body.

## 2. Recommended changes, in order

Each step is one measurable patch. Re-time proj7 after it. Stop when the profile says the next item is cold.

### P0. Attribute the time, then edit

Same binary, same flags as the 25.8 M/s run. Record the `[frame]` line (`%.2fM insns/s` from `vv_hw_summary`) and `[hw] cycles`. Default `--frame-clock` is wall (`vv_frame_clock` in `src/hw.c`). Pin it explicitly so a later run cannot switch clocks.

Useful A/B runs, same 50M cap, `--cpu=fast2` only:

- Baseline.
- `--no-irq` (span's `vv_irq_step`).
- `--quirks=no-load-use` (emulation off; `prep` stops arming `lu_on` through `any(LOAD_USE)`).
- `--quirks=no-dcache-stale` (`q_dc` off; `vv_dc_*` still called today).
- `--cpu=fast` and `--cpu=unicorn` as controls.

On Linux, `perf record -g -- ./vvizard --cpu=fast2 …` (or samply, see `docs/PROFILING-PLAN.md`) should show whether the samples sit in `span` / `exec_dec` / `do_mem` / `do_alu` / `vv_irq_step` / `vv_dc_load` / `vv_quirk_set_last_pc` / `vv_svc_quiet`. A `PROFILE=1` build counts `VV_PS_FAST2`, `VV_PS_IRQ`, `VV_PS_DC_LOAD`, `VV_PS_HW_TICK`. Do not publish a rate from the profiled binary.

### P1. Cheap span cleanups that do not change results

All in `src/cpu/fast2/exec.c` unless noted.

- `needs_tick`: for `OP_MEM`, return 1 when `(addr >> 28) == 8`. Delete the `vv_canon_inl` that follows. Keep using `gpr[rs]`, not `RR` (see risks).
- `vv_hw_set_now`: publish the instruction count when the span exits (the exit paths already do) and before an `vv_irq_step` that can take an interrupt. Skip the per-instruction call while `!vv_hw_irq_possible()`. `mfc0` of Count is `OP_MFC0`, which `is_fence` already refuses to fold into the span.
- IRQ early-out: when `fix_rt < 0`, COP0 emulation is on, the PC is outside the ISR nibble, and `!vv_hw_irq_possible()`, only shift the three last-PC words. Put that in `irq.h` as an inline, or a tiny function next to `vv_irq_step`, so the history `vv_irq_step` uses for branch deferral stays current. The full `vv_irq_step` remains the path when an interrupt can fire, inside the ISR, or `fix_rt` is live.
- `vv_quirk_set_last_pc`: call it when `(vv_quirk_detect >> UNMAPPED_FETCH) & 1`, and from `OP_FETCH0` as today. Default runs skip it. Cache the bit next to `q_fetch` in `vv_fast2_init`.

Leave `vv_svc_quiet` unless the profile shows the call itself. Inlining it means moving `insn_count` / `stop_req` / `note_ddr_exec` out of `src/main.c`. That touches the Unicorn hook. Do not do it in the first patch.

### P2. Make `prep` a bitmask, and split emulation from reporting

Today `Dec` (`src/cpu/fast2/decode.h`) has no read/write masks, so `prep` re-parses `d->insn` with `vv_reads` / `vv_value_reads` / `vv_writes`.

In `vv_decode_at`, store three masks on `Dec`: `reads`, `vreads` (`vv_value_reads`), `writes`. Fill them for every op, including ones that read nothing. Block build already calls `vv_decode_at` once per instruction. `BB_MAX * BB_SLOTS` grows by three words per slot; that cache is 4096 blocks.

`prep` then:

- `undef-reg`: `und = d->vreads & ~vv_reg_defined`. Keep the current "mark defined, report once, name one register" behavior. This is bookkeeping for the detector. `vv_reg_defined` is not an input to the ALU or to `RR`.
- Load-use emulation, `q_lu`: if `lu_on`, arm `lu_hold_sub` for this instruction and clear `lu_on`, the way `prep` does now, so `RR` returns `lu_old`. Do this even when the instruction does not read `lu_r`. An unused substitute does not change the result. `RR` already ignores it.
- Load-use report and the `qhits` increment: only when `(d->reads >> lu_r) & 1`. Use `vv_reads`, not `vv_value_reads` (stores read `rt`; `xor`/`sub`/`subu rd,rs,rs` still count as reads). Call `vv_quirk_hit` only when the detect bit is set, and return 1 before the `exec_dec` switch so a halt still happens before the use. When the detect bit is clear, increment the load-use counter with a direct `qhits` bump if the summary must stay aligned with Unicorn (see risks). Do not call `vv_gpr_name` on that path.

Cache `det_lu`, `det_undef`, and `det_fetch` in `vv_fast2_init` beside the existing `q_*` flags. Do not call `vv_quirk_active()` on this path.

Keep the old value out of `gpr[]`. `lu_make` records `old_rt` taken from `gpr[rt]` before `WR`, which is what the code does now (`uint32_t old_rt = gpr[d->rt]`). `RR` overlays it for one instruction. `vv_fast2_gpr` keeps returning `gpr[]`.

`nullify` returns before `prep` today, so an annulled delay slot does not consume `lu_on`. Preserve that order. Reserved is on by default, so `beql` and the other likely branches are nops, not annulled slots. The likely path is the `--quirks=no-reserved` case.

### P3. Straight-line handlers for the common ops

Keep `OP_MEM` / `OP_ALU` / `OP_IMM` as the decoded classes so `src/cpu/rtl/rtl.c` does not change. Add a short prefix inside the existing handlers.

ALU, at the top of `do_imm` / `do_alu`, before `alu()`:

- `A_ADDU`: `WR(rd or rt, RR(rs) + RR(rt or imm))`.
- `A_OR`, `A_AND`, `A_XOR`, `A_NOR`, `A_SUBU`, `A_SLL` / `A_SRL` / `A_SRA` with the shift amount already in `Dec`: the same one-line form fast uses.
- `A_ADD` / `A_SUB`: keep the overflow check and the `q_ov` wrap-vs-`exc(22)` split. Only skip the check when `q_ov` is on and the overflow detect bit is off; the wrap result is `a + b` either way. When the detect bit is on, keep `vv_quirk_hit` before the write, and return before `WR` on halt.

Memory, at the top of `do_mem`, handle and return:

- No `FL_ADDR0`, `FL_LWL`, `FL_LWR`, `FL_SWL`, `FL_SWR`.
- `size` is 4, or 1, or 2, and the address is aligned for that size.
- Raw nibble is not `0x2` when the ddr2 drop applies (store, PC outside BIOS, `q_ddr2`). Raw nibble is not `0x8`.
- `vv_canon_inl` hits a RAM host pointer (`vv_ram`).
- Then the existing D-cache block, a word load/store through `vv_ram_be32` / four explicit byte stores for `sw`, sign/zero extend for `lb`/`lh` with the current `q_lbsign` rule, `WR`, `lu_make`.

Everything else falls into the current ladder (unmapped, unaligned, MMIO, BIOS, `lwl`/`swl`, address 0, partial MMIO lane). Do not delete those paths.

`lb-sign` is off by default. The fast path uses the sign flag on `Dec` and `q_lbsign`, and it calls `vv_quirk_hit(LB_SIGN)` only when that detect bit is set.

### P4. Only if P0 says the D-cache calls dominate

`vv_dc_load` / `vv_dc_store` stay on the default path because `q_dc` is on. Gate the whole block on `q_dc || det_dc`. When both are off (`--quirks=no-dcache-stale` and no `--halt-on-quirk` / `--no-halt-on-quirk` / `--warn-on-quirk`), skip the calls. On a stale hit with detection on, keep today's order: `vv_dc_load` (the line fill is a side effect), then `vv_quirk_hit`, and return before `WR` if it halts.

Do not turn the model off to win the default benchmark. proj7 frames and `tests/hw/dcache*.s` depend on it.

### Leave these alone

- `src/cpu/fast.c`, `hook_code`, and Unicorn register fix-ups in `vv_quirk_pre` / `vv_quirk_post`.
- Merging the backends, or making fast2 call `exec_one`.
- `vv_svc_quiet_budget` and the rule that a pending IRQ with IE set ends the quiet span.
- Load-use emulation while the quirk is on. `--quirks=no-load-use` is a measurement switch, not the product default.
- RTL's use of the architectural address (`vv_fast2_gpr(rs) + imm`, no `RR`), documented at the top of `src/cpu/rtl/rtl.c`.

## 3. Correctness risks

### Load-delay emulation vs diagnostic bookkeeping

Emulation (must keep, default on):

- A load with `rt != 0` arms one next instruction: `RR(lu_r)` returns the pre-load `gpr` value. The load's own `WR` has already stored the new value.
- If that next instruction writes `rt`, the write stays. That is the "result wins" rule in the `load-use` quirk text.
- The consumer can be a delay slot. Consuming the hazard on the branch and then again on the slot would be wrong. `prep` clears `lu_on` on the first following `exec_dec`.
- IRQ entry calls `vv_fast2_cancel_lu` in both the outer loop and `span`. Keep both.
- `old_rt` is `gpr[rt]`, not `RR(rt)`. Using `RR` there chains the delay by an extra instruction.
- `vv_fast2_gpr` / `needs_tick` / RTL `dc_stall` read `gpr[]`, so they see the new value while `RR` is substituting. Keep that split. Writing the old value into `gpr[]` for the duration of the use would move MMIO fences and RTL D-cache lookups.

Bookkeeping (skip on the fast path when the detect bit is off; keep when it is on):

- `vv_value_reads` / `vv_writes` / `vv_reg_defined` exist only for `undef-reg`. With the bit off, skipping them does not change registers. With the bit on (`--halt-on-quirk`, `--warn-on-quirk`, or `--no-halt-on-quirk`), the once-per-register report must stay, including the store-data and `rd,rs,rs` exceptions in `vv_value_reads`.
- `load-use` reporting uses `vv_reads` and must fire only when the next instruction actually reads `rt`. `do_alu` calls `RR(rs)` even for `sll`, whose `rs` is not a source. Keying the report off "RR was called" false-positives. Halt still returns from `prep` before the handler.
- `vv_quirk_hit` increments `qhits` even when it does not print. Unicorn's `vv_quirk_pre` does the same, and `vv_quirk_needs_decode()` is true whenever a normal emulated quirk is on, so Unicorn still counts. `make test-fast2` compares stderr, and `[quirk] summary …` is not in `compare_runs.py`'s skip list. isr7 has real load-use hits (NOTES). A fast2 that stops counting them will fail `test-fast2` even though no line was printed. Either keep a direct increment on the same condition, or accept the summary diff and change the comparator in the same patch. Prefer the increment.
- `--no-halt-on-quirk` warns on `undef-reg` and `dcache-stale`. The fast path has to test the bits stored at init, not "the user passed no flag".

### RTL timing classifications

`vv_rtl_run` (`src/cpu/rtl/rtl.c`) calls `vv_fast2_init` and `vv_fast2_step`. Any change to `prep`, `do_mem`, `do_alu`, or `RR` changes RTL's architectural result in a `FIDELITY=rtl` build.

Stall classification is separate. `dc_stall` / `mem_dropped` re-decode and look at:

- `op == OP_MEM`
- `FL_STORE`, `FL_ADDR0`, `FL_LWL` / `FL_LWR` / `FL_SWL` / `FL_SWR`
- `alu` as the access size
- address = `vv_fast2_gpr(rs) + imm` (or 0 when reserved address-0 is on)
- the same drop rules as the quirks (unaligned, unmapped, BIOS store, ddr2 store, `lwl`/`swl`)

Adding `OP_LW` / `OP_SW` as new `op` values without updating `dc_stall` and `mem_dropped` zeroes those stalls. That moves Count, timer IRQs, and UART ready. P3 avoids that by keeping the opcode and specializing inside `do_mem`.

I-cache stalls use the PC nibble only. ALU fast paths do not affect them.

`needs_tick` must keep using `gpr[rs] + imm`. Substituting the load-delayed base would tick on a different instruction than today and would diverge from RTL's address.

### Quirk detection when halt or warn is on

`--halt-on-quirk` stops before the instruction (`vv_quirk_hit` returns 1, `exec_dec` returns 1, the handler must not have stored). Sites that already return before the side effect have to stay in that order:

- `prep` for `undef-reg` and `load-use`
- `do_alu` / `do_imm` for overflow, before `WR`
- `do_mem` for unmapped, unaligned, `lwlr`, BIOS, ddr2, MMIO lane, reserved address 0, `lb-sign`, and `dcache-stale`
- `do_hilo`, `do_cop0`, `do_eret`, `do_sys`, `do_rd0`, `OP_RI` / `OP_RNOP` / `OP_COP0X`

`--warn-on-quirk` logs once per `(quirk, pc)` and then executes the hardware result. Warning must not skip `lu_hold_sub` or the stale-data substitute.

`vv_quirk_not_emulated` (ddr2 load, reserved op in a delay slot, syscall in a delay slot) is a print, not a `vv_quirk_hit`. Leave those calls on the slow path.

Page-granularity `unmapped-fetch` reporting (`fetch_fill` plus `vv_quirk_fetch`) stays. It is not on the ALU/load hot path.

### Other behavioral traps

- A word fast path is big-endian only. `--cpu=fast2` already rejects `--little` in `main`. Use `vv_ram_be32`, not a host load.
- `lwl`/`lwr` merge uses `RR(rt)` and then `lu_make`. The fast path must not claim those flag combinations.
- Self-modifying stores call `note_store_code` → `bump_range` and `span_dead`. A fast store has to do that too, or a block will keep stale `Dec`s.
- `OP_FETCH0` still runs `vv_quirk_fetch` once per page. With detection off that only increments `qhits`. Leave it.

## 4. How to verify

Build the L0 binary: `make -j4 PROFILE=0 FIDELITY=l0`.

Functional, fast and Unicorn untouched so these should stay green:

- `make test` (vendor check, hex, hw, quirks, player, shared memory, threads). `test-quirks` and `test-hw` force `--cpu=unicorn`.
- `make test-fast2` (`tests/compare_runs.py`, Unicorn vs fast2, 2M instructions, hw programs, proj7 headless and the key script, event trace, PPM, stderr including `[quirk] summary`).

Quirk and halt, on fast2 itself. The existing quirk script hardcodes `--cpu=unicorn`, so run the same guest with `--cpu=fast2` and diff stderr against the unicorn line. Flags otherwise match `tests/test_quirks.py` `run()`: `--sync --no-window --frame-clock=emulated --quiet-mmio --no-random-ddr --max-insns 1000`, plus `--quiet-quirk=dcache-stale,tx-busy,rx-overrun,irq-storm,undef-reg` except where the case under test is one of those bugs.

- `tests/quirk/det_load_use`: `--halt-on-quirk=load-use` exits 4, one `[quirk] load-use at` line, and the use has not executed. `--warn-on-quirk=load-use` exits 0, one warning, and the register result matches the hardware (old) value. No halt/warn flag: no `[quirk] load-use at` line, and `--expect` still sees the old value (`emu_load_use` with the quirk on: `t3=1`, `t2=0x99`).
- `tests/quirk/det_load_use_ok`: `--halt-on-quirk=all` does not report `load-use`.
- `tests/quirk/emu_load_use`: `--quirks=+load-use` vs `--quirks=-load-use` match the on/off expects in `test_quirks.py`.
- `--halt-on-quirk=all` on the other `det_*.s` programs: same quirk name and exit 4 as Unicorn. `--halt-on-quirk` (no `=all`) does not halt on `jalr-rd` or `lb-sign`.
- `--no-halt-on-quirk` on a guest that reads a never-written GPR, and on `tests/hw` dcache stale: warning, exit not 4, hardware value still used. `--halt-on-quirk=undef-reg` and `--halt-on-quirk=dcache-stale` exit 4.
- IRQ: `tests/hw/irq_defer.s`, `irq_storm.s`, `irq_storm_eret.s` under `--cpu=fast2` vs Unicorn (event trace and exit). This is the P1 early-out.

RTL, after any `Dec` flag / `op` / memory-result change, and once at the end even if P3 kept `OP_MEM`:

- `make FIDELITY=rtl`
- `make test-cpu-rtl` if `CS150_ROOT` and iverilog are present (SKIP with exit 0 when they are not)
- `python3 tests/rtl/run_rtl_diff.py`

A functional fast path that preserves `Dec.op`, flags, `alu`, and `gpr[]` should keep stall counts. A new memory opcode will not.

Benchmark, no promised delta. Same host, `taskset` the way the 25.8 M/s number was taken, one warmup discarded, then several runs, report the median and the range.

```
./vvizard --cpu=fast2 --sync --no-window --max-insns 50000000
./vvizard --cpu=fast   --sync --no-window --max-insns 50000000
./vvizard --cpu=unicorn --sync --no-window --max-insns 50000000
```

Add every flag the original 50M run used (`--frame-clock`, `--random-ddr`, `--no-bios`, `--no-halt-on-quirk`). Read insn/s from the `[frame]` line and confirm `[hw] cycles` still equals the instruction count on L0. Compare registers / UART / the frame PPM against the pre-change fast2 binary on a short run before trusting a faster number (`--dump-frame`, `--max-insns` well under 50M).

## 5. What this plan does not claim

No speedup factor. P0 decides whether P1 (call overhead), P2 (re-decode in `prep`), or P3 (`do_mem` / `alu`) is the actual 1.9×. Ship them as separate changes so a regression bisects. If a step does not move the median outside the run-to-run range, drop it.
