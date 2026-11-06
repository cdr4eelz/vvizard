/* irq.c - the DO_ISR decision that used to live in main.c irq_step.  Unicorn and the fast
 * backend both come through here so EPC, the IE clear and the deferral rules stay one piece of code. */
#include "irq.h"
#include "cpu/mips_decode.h"
#include "cpu/fast/fast.h"
#include "hw.h"
#include "quirk.h"
#include "memmap.h"
#include "profile.h"

uint64_t vv_irq_count;

#define NOPC 0xFFFFFFFFu
static uint32_t last_pc = NOPC, last2_pc = NOPC, last3_pc = NOPC;
static uint32_t prev_status;
static int fix_rt = -1;
static uint32_t fix_val;

static uint32_t R(uc_engine *uc, unsigned r) {
    if (!r) return 0;
    if (!uc) return vv_fast_gpr(r);
    uint32_t v = 0;
    uc_reg_read(uc, UC_MIPS_REG_0 + r, &v);
    return v;
}
static void W(uc_engine *uc, unsigned r, uint32_t v) {
    if (!r) return;
    if (!uc) { vv_fast_set_gpr(r, v); return; }
    uc_reg_write(uc, UC_MIPS_REG_0 + r, &v);
}
static uint32_t ST(uc_engine *uc) {
    if (vv_quirk_on(VV_Q_COP0)) return vv_hw_status();
    if (!uc) return vv_fast_c0();
    uint32_t s = 0;
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &s);
    return s;
}
static void set_st(uc_engine *uc, uint32_t s) {
    if (vv_quirk_on(VV_Q_COP0)) return;          /* shadow Status is already updated by vv_hw_isr_enter */
    if (!uc) { vv_fast_set_c0(s); return; }
    uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &s);
}
static uint32_t read_insn(uint32_t a) {
    int ok;
    return vv_ram_be32(vv_canon_inl(a), &ok);
}

int vv_irq_step(uc_engine *uc, uint32_t pc, uint32_t *new_pc) {
    VV_PROF_COUNT(VV_PS_IRQ);
    int cop0 = vv_quirk_on(VV_Q_COP0);
    if (fix_rt > 0) W(uc, (unsigned)fix_rt, fix_val);
    fix_rt = -1;
    /* cop0 quirk off: mfc0/mtc0 Cause/EPC inside the ISR BRAM are patched onto the hw.c latch.
     * The write lands at the next boundary, before that next instruction runs (same as before). */
    if (!cop0 && (pc >> 28) == 0xC) {
        uint32_t i = read_insn(pc);
        if ((i >> 26) == 0x10 && (i & 0x7FF) == 0) {
            unsigned rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31;
            if (rs == 0 && (rd == 13 || rd == 14)) {
                fix_rt = (int)rt;
                fix_val = rd == 14 ? vv_hw_epc() : vv_hw_cause();
            } else if (rs == 4 && (rd == 13 || rd == 14)) {
                uint32_t v = R(uc, rt);
                if (rd == 13) vv_cop0_write(13, v);
                else vv_hw_set_epc(v);
            }
        }
    }
    if (!vv_hw_irq_possible()) {
        if (vv_quirk_halted) return 0;
        last3_pc = last2_pc; last2_pc = last_pc; last_pc = pc;
        return 0;
    }
    uint32_t st = ST(uc);
    if ((st & 1) && !(prev_status & 1)) vv_hw_isr_exit_check(st, pc, "IE re-enabled");
    prev_status = st;
    if (vv_quirk_halted) return 0;
    if (vv_hw_irq_request(st)) {
        int defer = 0;
        if (last_pc != NOPC) {
            uint32_t x = read_insn(last_pc);
            defer = vv_is_branch(x) || vv_is_mtc0(x);
        }
        if (!defer && last3_pc != NOPC && vv_is_branch(read_insn(last3_pc)) && last_pc != last3_pc + 8)
            defer = 1;
        if (!defer) {
            VV_EVT("irq-enter cause 0x%04x status 0x%08x", vv_hw_cause() & 0xFFFFu, st);
            vv_hw_isr_enter(pc, st);
            st &= ~1u;
            prev_status = st;
            set_st(uc, st);
            if (new_pc) *new_pc = VV_PC_ISR;
            vv_irq_count++;
            last_pc = last2_pc = last3_pc = NOPC;
            return 1;
        }
    }
    last3_pc = last2_pc; last2_pc = last_pc; last_pc = pc;
    return 0;
}
