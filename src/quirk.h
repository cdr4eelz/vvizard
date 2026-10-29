/* quirk.h - --halt-on-quirk / --warn-on-quirk: detect guest behaviour whose result differs
 * between the real MIPS150 Verilog CPU (hardware/src) and standard MIPS (what Unicorn runs). */
#ifndef VV_QUIRK_H
#define VV_QUIRK_H
#include <unicorn/unicorn.h>
#include <stdint.h>

/* name, hardware behaviour (one line, shown in the diagnostic) */
#define VV_QUIRK_TABLE \
    Q(JALR_RD,    "jalr-rd",    "jalr with rd != $31; HW always links to $31 (InstructionControl.v:113-116)") \
    Q(UNMAPPED,   "unmapped",   "access to an undecoded address; HW reads/fetches 0 and drops writes (MemBank.v:104-133)") \
    Q(BIOS_STORE, "bios-store", "store into the BIOS region; HW BIOS is data-read-only, the store is dropped (MemBank.v:75)") \
    Q(UNALIGNED,  "unaligned",  "unaligned lw/lh/lhu/sw/sh; HW loads 0 / drops the store, no AdEL/AdES (ByteAccess4.v:28-57)") \
    Q(LWLR,       "lwlr",       "lwl/lwr/swl/swr; HW loads 0 into rt / stores nothing (ByteAccess4.v default case)") \
    Q(OVERFLOW,   "overflow",   "add/addi/sub signed overflow; HW wraps (no Ov trap, ALUdec.v:33-36)") \
    Q(HILO,       "hilo",       "mult/div/mfhi/mflo/mthi/mtlo; HW has no HI/LO (ALU_XXX: mf* write 0, others no-op)") \
    Q(SYSCALL,    "syscall",    "syscall; HW executes it as a NOP (no exceptions)") \
    Q(UART_LANE,  "uart-lane",  "sb/sh to UART TX not covering byte lane +3; HW transmits dina[7:0] = 0x00 (MemMapIO.v:94-97)") \
    Q(LOAD_USE,   "load-use",   "insn after a load reads the load's rt; HW has no interlock, reads the OLD value (StageMW.v:72-74)")

enum {
#define Q(id, n, d) VV_Q_##id,
    VV_QUIRK_TABLE
#undef Q
    VV_Q_COUNT
};

extern uint32_t vv_quirk_mask;   /* bit per category; 0 = feature off (default) */
extern int vv_quirk_warn;        /* 1: log each (category, pc) once and keep running */
extern int vv_quirk_halted;      /* set once a halt-mode quirk stopped the run */

/* Parse "all", "a,b,c", "all,-load-use", "-load-use" (leading '-' starts from all). 0 ok, -1 bad. */
int  vv_quirk_parse(const char *list);
const char *vv_quirk_names(void);   /* "jalr-rd,unmapped,..." for --help */
/* Called from the code hook. post(): hook after the PREVIOUS insn (currently no checks;
 * lb/lh sign-extension is not a quirk: the Verilog will be fixed).  pre(): decodes the insn about to execute at pc. */
void vv_quirk_post(uc_engine *uc, uint32_t pc);
void vv_quirk_pre(uc_engine *uc, uint32_t pc);
void vv_quirk_cancel(void);         /* PC was redirected (IRQ entry): drop pending load state */
/* Called from the TLB fill hook on an instruction fetch that hardware would decode as 0. */
void vv_quirk_fetch(uc_engine *uc, uint32_t va, const char *why);
void vv_quirk_summary(void);

/* provided by main.c */
uint64_t vv_insns(void);           /* instructions executed so far */
void vv_dump_regs(uc_engine *uc);
uint32_t vv_canon(uint32_t a);      /* canonical address, 0xFFFFFFFF if undecoded */
#endif
