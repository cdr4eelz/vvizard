/* quirk.h - MIPS150 hardware quirks: behaviour where the real MIPS150 Verilog CPU (hardware/src)
 * differs from standard MIPS32 (what Unicorn executes).
 *
 * Each quirk can be
 *   EMULATED (--quirks=LIST, env VVIZARD_QUIRKS, make HW_QUIRKS=...): vvizard reproduces the
 *     hardware result instead of Unicorn's.  Default: the quirks the CURRENT hardware has
 *     (default column below); the two fixed bugs (jalr-rd, lb-sign) are off but available.
 *   REPORTED (--halt-on-quirk[=LIST] / --warn-on-quirk[=LIST]): stop / log when the guest hits
 *     one.  The default LIST is the emulated set.  The guest-bug detectors (bug column) are
 *     reported as warnings even without a flag; --quiet-quirk=LIST silences any category.
 * References are file:line in hardware/src (commit fcdda7f). */
#ifndef VV_QUIRK_H
#define VV_QUIRK_H
#include <unicorn/unicorn.h>
#include <stdint.h>
#include <stdio.h>

/* id, name, default (1 = current hardware has it), bug (1 = guest-bug detector: reported as a warning
 * by default), hardware behaviour (shown in diagnostics).  For tx-busy, gp-busy, rx-overrun, irq-storm and
 * undef-reg the hardware behaviour is always modelled (src/hw.c; --legacy-io turns off the UART
 * timing); only dcache-stale has an emulation switch. */
#define VV_QUIRK_TABLE \
    Q(BAL_UNTAKEN, "bal-untaken", 1, 0, "an untaken bltzal/bgezal does not link: $31 keeps its old value (cpu/StageDX.v:105)") \
    Q(LOAD_USE, "load-use", 1, 0, "no load interlock: the insn right after a load reads the OLD rt; if it writes rt itself, its result wins over the load (cpu/StageMW.v:75-76 WBK_CanFWD=0, cpu/CPUMIPS.v:131)") \
    Q(UNMAPPED, "unmapped", 1, 0, "load/store to an undecoded nibble (0x0,0x7,0x9-0xB,0xD-0xF): load reads 0, store is dropped, no exception (cpu/MemBank.v:69-83,132 DEAD_DMEM=0)") \
    Q(UNMAPPED_FETCH, "unmapped-fetch", 1, 0, "fetch from an undecoded nibble or from MMIO (0x8) reads 0, i.e. runs nop (cpu/MemBank.v:104-113 DEAD_IMEM=0)") \
    Q(BIOS_STORE, "bios-store", 1, 0, "a store into the BIOS (0x4xxxxxxx) is dropped: BIOS is data-read-only (cpu/MemBank.v:75)") \
    Q(DDR2, "ddr2", 1, 0, "0x2xxxxxxx is the I-cache path: a store from code outside the BIOS is dropped (cpu/MemBank.v:80); a load returns the D-cache's stale output (cpu/MemBank.v:130, not emulated)") \
    Q(UNALIGNED, "unaligned", 1, 0, "unaligned lw/lh/lhu/sw/sh: the load writes 0, the store is dropped, no AdEL/AdES; on MMIO the mask-0 access is a read, so it pops UART RX (innerds/ByteAccess4.v:36,69, cpu/MemMapIO.v:65-66,97)") \
    Q(LWLR, "lwlr", 1, 0, "lwl/lwr write 0 to rt, swl/swr store nothing (MemShift=1 hits the ByteAccess4 default case; on MMIO acts as a read) (innerds/ByteAccess4.v:36,69)") \
    Q(OVERFLOW, "overflow", 1, 0, "add/addi/sub signed overflow wraps, no Ov trap (innerds/ALUdec.v:33-36,61)") \
    Q(HILO, "hilo", 1, 0, "no HI/LO: mult/multu/div/divu/mthi/mtlo do nothing, mfhi/mflo write 0 (innerds/ALUdec.v:44-47, ALU_XXX output is 0)") \
    Q(SYSCALL, "syscall", 1, 0, "syscall is a nop, no exception (innerds/ALUdec.v:44)") \
    Q(RESERVED, "reserved", 1, 0, "insns the MIPS150 does not decode: unknown SPECIAL funct (movz/movn/sync/teq...) writes 0 to rd; branch-likely, SPECIAL2/3 (mul/clz), COP1-3, LL/SC/LWC/SWC/PREF, COP0 ops other than mfc0/mtc0 (eret, di/ei) are nops; rotr/rotrv act as srl/srlv; REGIMM rt other than 0/1/16/17 acts as bltz/bgez(al) by rt[0]/rt[4] (traps branch!); ops 0x27/0x2C/0x2D/0x2F access address 0 (innerds/InstructionControl.v:33,58-59,69,86,125-127, innerds/ALUdec.v:22,75)") \
    Q(COP0, "cop0", 1, 0, "COP0150 has only Count(9, +1 per cycle), Compare(11, reset 0xFFFF), Status(12, reset 0, all 32 bits RW), Cause(13), EPC(14); mtc0 EPC is ignored, other registers read 0, the sel field is ignored (cpu/COP0150.v:24-28,61,70-75)") \
    Q(MMIO_LANE, "mmio-lane", 1, 0, "sb/sh to an MMIO register delivers the ByteAccess4 lane-placed word (data in its big-endian lane, zeros elsewhere): UART TX and the LCD chars (0x80000100+) take dina[7:0], so sb not at +3 sends/writes 0x00 (innerds/ByteAccess4.v WordMasked, cpu/MemMapIO.v, base_io/LCDText2x16.v)") \
    Q(JALR_RD, "jalr-rd", 0, 0, "FIXED in fcdda7f (old hardware): jalr linked to $31 whatever rd was (innerds/InstructionControl.v:115)") \
    Q(LB_SIGN, "lb-sign", 0, 0, "FIXED in fcdda7f (old hardware): lb/lh zero-extended like lbu/lhu (innerds/ByteAccess4.v ValExtract)") \
    Q(DCACHE_STALE, "dcache-stale", 1, 1, "the D-cache (direct mapped, 256 x 32-byte lines, write-through, no invalidate) is not updated by GPU writes to DDR: a CPU load from a line it cached before the GPU wrote it returns the old data (base_mem/Cache.v, cache.vh; src/dcache.c)") \
    Q(TX_BUSY, "tx-busy", 1, 1, "a UART TX_DATA store while TX is busy (TX_READY=0 for 4773 cycles after the previous byte) is lost (cpu/MemMapIO.v:101, base_io/UATransmit.v:30)") \
    Q(RX_OVERRUN, "rx-overrun", 1, 1, "UART RX holds one byte: a byte that lands before the previous one was read overwrites it (base_io/UAReceive.v:103-107)") \
    Q(IRQ_STORM, "irq-storm", 1, 1, "the ISR returns (re-enables Status.IE, or runs eret) while the Cause bit that caused the interrupt is still set and enabled: COP0150 interrupts again right away (cpu/COP0150.v:56,91)") \
    Q(GP_BUSY, "gp-busy", 1, 1, "a GP_CODE store while the GPU is busy (GP_STATE gp_ready=0) is dropped: GraphicsProcessor starts only on GP_ready && GP_vcode (gpu/GraphicsProcessor.v T_START, cpu/MemMapIO.v gp_vcode); wait with GP_WAIT() (software/common/graphics.h).  Only with --threads=on: the --sync GPU is always ready") \
    Q(UNDEF_REG, "undef-reg", 1, 1, "an instruction reads a GPR never written since reset: the register file is distributed RAM without reset (0 after FPGA configuration, stale after a reset); vvizard no longer presets $sp (innerds/RegFile.v:38-41)")

enum {
#define Q(id, n, def, bug, d) VV_Q_##id,
    VV_QUIRK_TABLE
#undef Q
    VV_Q_COUNT
};

extern uint32_t vv_quirk_emul;   /* bit per quirk: emulated (hardware behaviour) */
extern uint32_t vv_quirk_detect; /* bit per quirk: reported */
extern uint32_t vv_quirk_halt;   /* bit per quirk: reporting it stops the run (--halt-on-quirk); others log each (quirk, pc) once */
extern int vv_quirk_halted;      /* set once a halt-mode quirk stopped the run */
#define vv_quirk_on(id)  ((vv_quirk_emul >> (id)) & 1u)
#define vv_quirk_active() (vv_quirk_emul | vv_quirk_detect)
/* Which quirks need the per-instruction decode (vv_quirk_post / vv_quirk_pre in the code hook).
 * Not needed: tx-busy, gp-busy and rx-overrun (detected in the UART model, src/hw.c), irq-storm (Status.IE
 * edge in irq_step; its eret case is vv_quirk_pre_lite, which reads only ISR instructions) and
 * unmapped-fetch (tlb_fill).  Their emulation switches do nothing per instruction either, nor does
 * undef-reg's (the register file model has no switch).  Everything else, emulated or detected,
 * is decoded in pre(): undef-reg and dcache-stale detection, load-use, mmio-lane detection (its
 * emulation is in mmio_write, but pre() counts its hits), and the other emulated quirks. */
#define VV_Q_BIT(id) (1u << VV_Q_##id)
#define VV_Q_NODECODE_DET  (VV_Q_BIT(TX_BUSY) | VV_Q_BIT(GP_BUSY) | VV_Q_BIT(RX_OVERRUN) | VV_Q_BIT(IRQ_STORM) | VV_Q_BIT(UNMAPPED_FETCH))
#define VV_Q_NODECODE_EMUL (VV_Q_NODECODE_DET | VV_Q_BIT(UNDEF_REG))
#define vv_quirk_needs_decode() \
    (((vv_quirk_emul & ~VV_Q_NODECODE_EMUL) | (vv_quirk_detect & ~VV_Q_NODECODE_DET)) != 0)
extern int vv_quirk_decode;      /* vv_quirk_needs_decode(), set before the run */

uint32_t vv_quirk_default(void);    /* the current-hardware set */
uint32_t vv_quirk_bugs(void);       /* the guest-bug detectors */
/* Parse a comma list into *out.  The list edits `base` left to right: NAME and +NAME add,
 * -NAME and no-NAME remove; the keywords none, all and default (= `dflt`) replace the set so far
 * (-all / no-default remove those bits).  Returns 0 ok, -1 bad (message printed). */
int  vv_quirk_parse(const char *list, uint32_t base, uint32_t dflt, uint32_t *out);
int  vv_quirk_list_ok(const char *list);   /* 1 if every token is a known keyword / quirk (silent) */
const char *vv_quirk_names(void);   /* "bal-untaken,load-use,..." */
const char *vv_quirk_set_str(uint32_t m);  /* names in m, or "none" */
void vv_quirk_list(FILE *f);        /* --list-quirks table */
/* Code-hook entry points. post(): finish the PREVIOUS insn (apply emulation fix-ups);
 * pre(): decode the insn about to execute at pc (report, set up emulation; may skip it). */
int  vv_quirk_post(uc_engine *uc, uint32_t pc);   /* 1: PC redirected (emulated branch); insn at pc not run */
void vv_quirk_pre(uc_engine *uc, uint32_t pc);
/* Instead of post/pre when no quirk needs the decode but some detector is on: only the eret
 * irq-storm check (inside an ISR) and the last-PC note for unmapped-fetch reports. */
void vv_quirk_pre_lite(uc_engine *uc, uint32_t pc);
void vv_quirk_cancel(void);         /* PC was redirected (IRQ entry): drop pending state */
/* TLB fill hook on an instruction fetch that hardware would decode as 0. */
void vv_quirk_fetch(uc_engine *uc, uint32_t va, const char *why);
void vv_quirk_summary(void);
/* Report category cat at pc (devices, src/hw.c).  Returns 1 if the run halted. */
int  vv_quirk_report(int cat, uint32_t pc, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void vv_quirk_reg_defined(unsigned r);
void vv_quirk_init(uc_engine *uc);   /* the host set GPR r (e.g. --sp): not undefined */

/* VOID page: undecoded addresses (unmapped / unmapped-fetch) and redirected accesses
 * (unaligned, lwlr) land here; it reads 0 and is re-zeroed after every store. */
#define VV_VOID_PA   0xF0000000u
#define VV_VOID_VA   0xF0000800u    /* redirect target (nibble 0xF is undecoded) */

/* provided by main.c */
uint64_t vv_insns(void);           /* instructions executed so far */
void vv_dump_regs(uc_engine *uc);
uint32_t vv_canon(uint32_t a);      /* canonical address, 0xFFFFFFFF if undecoded */
uint32_t vv_cop0_read(unsigned rd); /* COP0150 register model (quirk cop0) */
void vv_cop0_write(unsigned rd, uint32_t v);
void vv_mmio_side_read(uint32_t canon); /* side effect of a hardware MMIO read (UART RX pop) */
#endif
