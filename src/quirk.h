/* quirk.h - MIPS150 hardware quirks: behaviour where the real MIPS150 Verilog CPU (hardware/src)
 * differs from standard MIPS32 (what Unicorn executes).
 *
 * Each quirk can be
 *   EMULATED (--quirks=LIST, env VVIZARD_QUIRKS, make HW_QUIRKS=...): vvizard reproduces the
 *     hardware result instead of Unicorn's.  Default: the quirks the CURRENT hardware has
 *     (default column below); the two fixed bugs (jalr-rd, lb-sign) are off but available.
 *   REPORTED (--halt-on-quirk[=LIST] / --warn-on-quirk[=LIST]): stop / log when the guest hits
 *     one.  The default LIST is the emulated set.
 * References are file:line in hardware/src (commit fcdda7f). */
#ifndef VV_QUIRK_H
#define VV_QUIRK_H
#include <unicorn/unicorn.h>
#include <stdint.h>
#include <stdio.h>

/* id, name, default (1 = current hardware has it), hardware behaviour (shown in diagnostics) */
#define VV_QUIRK_TABLE \
    Q(BAL_UNTAKEN, "bal-untaken", 1, "an untaken bltzal/bgezal does not link: $31 keeps its old value (cpu/StageDX.v:105)") \
    Q(LOAD_USE,    "load-use",    1, "no load interlock: the insn right after a load reads the OLD rt; if it writes rt itself, its result wins over the load (cpu/StageMW.v:75-76 WBK_CanFWD=0, cpu/CPUMIPS.v:131)") \
    Q(UNMAPPED,    "unmapped",    1, "load/store to an undecoded nibble (0x0,0x7,0x9-0xB,0xD-0xF): load reads 0, store is dropped, no exception (cpu/MemBank.v:69-83,132 DEAD_DMEM=0)") \
    Q(UNMAPPED_FETCH, "unmapped-fetch", 1, "fetch from an undecoded nibble or from MMIO (0x8) reads 0, i.e. runs nop (cpu/MemBank.v:104-113 DEAD_IMEM=0)") \
    Q(BIOS_STORE,  "bios-store",  1, "a store into the BIOS (0x4xxxxxxx) is dropped: BIOS is data-read-only (cpu/MemBank.v:75)") \
    Q(DDR2,        "ddr2",        1, "0x2xxxxxxx is the I-cache path: a store from code outside the BIOS is dropped (cpu/MemBank.v:80); a load returns the D-cache's stale output (cpu/MemBank.v:130, not emulated)") \
    Q(UNALIGNED,   "unaligned",   1, "unaligned lw/lh/lhu/sw/sh: the load writes 0, the store is dropped, no AdEL/AdES; on MMIO the mask-0 access is a read, so it pops UART RX (innerds/ByteAccess4.v:36,69, cpu/MemMapIO.v:65-66,97)") \
    Q(LWLR,        "lwlr",        1, "lwl/lwr write 0 to rt, swl/swr store nothing (MemShift=1 hits the ByteAccess4 default case; on MMIO acts as a read) (innerds/ByteAccess4.v:36,69)") \
    Q(OVERFLOW,    "overflow",    1, "add/addi/sub signed overflow wraps, no Ov trap (innerds/ALUdec.v:33-36,61)") \
    Q(HILO,        "hilo",        1, "no HI/LO: mult/multu/div/divu/mthi/mtlo do nothing, mfhi/mflo write 0 (innerds/ALUdec.v:44-47, ALU_XXX output is 0)") \
    Q(SYSCALL,     "syscall",     1, "syscall is a nop, no exception (innerds/ALUdec.v:44)") \
    Q(RESERVED,    "reserved",    1, "insns the MIPS150 does not decode: unknown SPECIAL funct (movz/movn/sync/teq...) writes 0 to rd; branch-likely, SPECIAL2/3 (mul/clz), COP1-3, LL/SC/LWC/SWC/PREF, COP0 ops other than mfc0/mtc0 (eret, di/ei) are nops; rotr/rotrv act as srl/srlv; REGIMM rt other than 0/1/16/17 acts as bltz/bgez(al) by rt[0]/rt[4] (traps branch!); ops 0x27/0x2C/0x2D/0x2F access address 0 (innerds/InstructionControl.v:33,58-59,69,86,125-127, innerds/ALUdec.v:22,75)") \
    Q(COP0,        "cop0",        1, "COP0150 has only Count(9, +1 per cycle), Compare(11, reset 0xFFFF), Status(12, reset 0, all 32 bits RW), Cause(13), EPC(14); mtc0 EPC is ignored, other registers read 0, the sel field is ignored (cpu/COP0150.v:24-28,61,70-75)") \
    Q(MMIO_LANE,   "mmio-lane",   1, "sb/sh to an MMIO register delivers the ByteAccess4 lane-placed word (data in its big-endian lane, zeros elsewhere): UART TX sends dina[7:0], so sb not at +3 sends 0x00 (innerds/ByteAccess4.v WordMasked, cpu/MemMapIO.v:99)") \
    Q(JALR_RD,     "jalr-rd",     0, "FIXED in fcdda7f (old hardware): jalr linked to $31 whatever rd was (innerds/InstructionControl.v:115)") \
    Q(LB_SIGN,     "lb-sign",     0, "FIXED in fcdda7f (old hardware): lb/lh zero-extended like lbu/lhu (innerds/ByteAccess4.v ValExtract)")

enum {
#define Q(id, n, def, d) VV_Q_##id,
    VV_QUIRK_TABLE
#undef Q
    VV_Q_COUNT
};

extern uint32_t vv_quirk_emul;   /* bit per quirk: emulated (hardware behaviour) */
extern uint32_t vv_quirk_detect; /* bit per quirk: reported by --halt-on-quirk / --warn-on-quirk */
extern int vv_quirk_warn;        /* 1: log each (quirk, pc) once and keep running */
extern int vv_quirk_halted;      /* set once a halt-mode quirk stopped the run */
#define vv_quirk_on(id)  ((vv_quirk_emul >> (id)) & 1u)
#define vv_quirk_active() (vv_quirk_emul | vv_quirk_detect)

uint32_t vv_quirk_default(void);    /* the current-hardware set */
/* Parse a comma list into *out, starting from `base` if the list begins with '-' or '+'.
 * Tokens: default, none, all, NAME, -NAME, +NAME. Returns 0 ok, -1 bad. */
int  vv_quirk_parse(const char *list, uint32_t base, uint32_t *out);
const char *vv_quirk_names(void);   /* "bal-untaken,load-use,..." */
const char *vv_quirk_set_str(uint32_t m);  /* names in m, or "none" */
void vv_quirk_list(FILE *f);        /* --list-quirks table */
/* Code-hook entry points. post(): finish the PREVIOUS insn (apply emulation fix-ups);
 * pre(): decode the insn about to execute at pc (report, set up emulation; may skip it). */
int  vv_quirk_post(uc_engine *uc, uint32_t pc);   /* 1: PC redirected (emulated branch); insn at pc not run */
void vv_quirk_pre(uc_engine *uc, uint32_t pc);
void vv_quirk_cancel(void);         /* PC was redirected (IRQ entry): drop pending state */
/* TLB fill hook on an instruction fetch that hardware would decode as 0. */
void vv_quirk_fetch(uc_engine *uc, uint32_t va, const char *why);
void vv_quirk_summary(void);

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
