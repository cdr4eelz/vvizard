/* decode.c - classify one instruction.  Runtime register values are not consulted; branches,
 * overflow and memory quirks are resolved when the instruction runs. */
#include "decode.h"
#include "fast.h"
#include "memmap.h"
#include "quirk.h"
#include "cpu/mips_decode.h"

uint32_t vv_fast_fetch_raw(uint32_t pc, int *dead) {
    uint32_t c = vv_canon_inl(pc);
    int hole = c == 0xFFFFFFFFu || (c >> 28) == 0x8 || (pc & 3);
    if (dead) *dead = hole;
    if (hole) return 0;
    int ok;
    return vv_ram_be32(c, &ok);
}

int vv_fast_peek_mem(uint32_t pc, uint32_t *addr, int *store) {
    int dead = 0;
    uint32_t i = vv_fast_fetch_raw(pc, &dead);
    if (dead) return 0;
    unsigned op = i >> 26;
    if (op < 0x20 || op > 0x2F) return 0;
    unsigned rs = (i >> 21) & 31;
    int32_t imm = (int16_t)(i & 0xFFFF);
    if (addr) *addr = vv_fast_gpr(rs) + (uint32_t)imm;
    if (store) *store = op >= 0x28;
    return 1;
}

static int q(int id) { return vv_quirk_on(id); }

void vv_decode_at(uint32_t pc, Dec *d) {
    int dead = 0;
    uint32_t i = vv_fast_fetch_raw(pc, &dead);
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31;
    unsigned sa = (i >> 6) & 31, fn = i & 63;
    int32_t imm = (int16_t)(i & 0xFFFF);
    *d = (Dec){0};
    d->insn = i; d->pc = pc; d->rs = (uint8_t)rs; d->rt = (uint8_t)rt; d->rd = (uint8_t)rd; d->sa = (uint8_t)sa; d->imm = imm;
    if (dead) {
        d->op = (pc & 3) || !q(VV_Q_UNMAPPED_FETCH) ? OP_FETCHX : OP_FETCH0;
        if ((pc & 3) == 0 && q(VV_Q_UNMAPPED_FETCH)) d->op = OP_FETCH0;
        if (pc & 3) d->op = OP_FETCHX;           /* misaligned PC is AdEL on L0, even with the quirk */
        return;
    }
    if (op == 0) {
        if (fn == 8 || fn == 9) { d->op = OP_JR; d->flags = fn == 9 ? FL_LINK : 0; return; }
        if (fn == 12) { d->op = OP_SYSCALL; return; }
        if (fn == 13) { d->op = OP_BREAK; return; }
        if (fn == 16 || fn == 18 || fn == 17 || fn == 19 || (fn >= 24 && fn <= 27)) {
            d->op = OP_HILO;
            d->alu = fn == 16 ? H_MFHI : fn == 18 ? H_MFLO : fn == 17 ? H_MTHI : fn == 19 ? H_MTLO :
                     fn == 24 ? H_MULT : fn == 25 ? H_MULTU : fn == 26 ? H_DIV : H_DIVU;
            return;
        }
        if (fn == 10 || fn == 11) {                 /* movz / movn: reserved on this CPU */
            d->op = q(VV_Q_RESERVED) ? OP_RD0 : OP_MOVZ;
            d->alu = fn == 11;
            return;
        }
        if (fn == 2 && (rs & 1)) { d->op = OP_ALU; d->alu = A_ROTR; return; }
        if (fn == 6 && (sa & 1)) { d->op = OP_ALU; d->alu = A_ROTRV; return; }
        if (!vv_known_special(fn)) { d->op = q(VV_Q_RESERVED) ? OP_RD0 : OP_RI; return; }
        d->op = OP_ALU;
        switch (fn) {
        case 0: d->alu = A_SLL; break;
        case 2: d->alu = A_SRL; break;
        case 3: d->alu = A_SRA; break;
        case 4: d->alu = A_SLLV; break;
        case 6: d->alu = A_SRLV; break;
        case 7: d->alu = A_SRAV; break;
        case 32: d->alu = A_ADD; break;
        case 33: d->alu = A_ADDU; break;
        case 34: d->alu = A_SUB; break;
        case 35: d->alu = A_SUBU; break;
        case 36: d->alu = A_AND; break;
        case 37: d->alu = A_OR; break;
        case 38: d->alu = A_XOR; break;
        case 39: d->alu = A_NOR; break;
        case 42: d->alu = A_SLT; break;
        case 43: d->alu = A_SLTU; break;
        default: d->op = OP_NOP; break;
        }
        return;
    }
    if (op == 1) {
        int likely = rt == 2 || rt == 3 || rt == 18 || rt == 19;
        int link = (rt & 0x10) != 0;
        int trap = rt >= 8 && rt != 16 && rt != 17 && !likely;
        if (trap && !q(VV_Q_RESERVED)) { d->op = OP_TRAP; return; }
        d->op = OP_B; d->alu = (rt <= 1 || (q(VV_Q_RESERVED) && rt > 1)) ? B_REGIMM : (rt & 1) ? B_GEZ : B_LTZ;
        if (rt <= 1) d->alu = (rt & 1) ? B_GEZ : B_LTZ;
        else if (q(VV_Q_RESERVED)) d->alu = B_REGIMM;
        else d->alu = (rt & 1) ? B_GEZ : B_LTZ;
        if (link) d->flags |= FL_LINK;
        if (likely) d->flags |= FL_LIKELY;
        return;
    }
    if (op == 2 || op == 3) {
        d->op = OP_J; d->imm = (int32_t)((pc + 4) & 0xF0000000u) | (int32_t)((i & 0x03FFFFFFu) << 2);
        if (op == 3) d->flags |= FL_LINK;
        return;
    }
    if (op == 4 || op == 5) { d->op = OP_B; d->alu = op == 4 ? B_EQ : B_NE; return; }
    if (op == 6 || op == 7) { d->op = OP_B; d->alu = op == 6 ? B_LEZ : B_GTZ; return; }
    if (op >= 8 && op <= 15) {
        if (op == 15) { d->op = OP_LUI; d->imm = (int32_t)((i & 0xFFFF) << 16); return; }
        d->op = OP_IMM;
        d->alu = op == 8 ? A_ADD : op == 9 ? A_ADDU : op == 10 ? A_SLT : op == 11 ? A_SLTU :
                 op == 12 ? A_AND : op == 13 ? A_OR : A_XOR;
        if (op >= 12 && op <= 14) d->imm = (int32_t)(i & 0xFFFF);   /* zero-extended */
        return;
    }
    if (op == 0x10) {
        if (rs == 0) { d->op = OP_MFC0; return; }
        if (rs == 4) { d->op = OP_MTC0; return; }
        if (i == 0x42000018u) { d->op = OP_ERET; return; }
        d->op = OP_COP0X;                    /* eret is the only COP0 op Unicorn runs; the rest are RI */
        return;
    }
    if (op >= 0x14 && op <= 0x17 && !q(VV_Q_RESERVED)) {
        /* beql/bnel/blezl/bgtzl: real branch-likely when the reserved quirk is off. */
        d->op = OP_B;
        d->alu = op == 0x14 ? B_EQ : op == 0x15 ? B_NE : op == 0x16 ? B_LEZ : B_GTZ;
        d->flags |= FL_LIKELY;
        return;
    }
    if ((op >= 0x11 && op <= 0x1F) || op >= 0x30) {
        /* ll/sc/pref/COP1/...: hardware nop. Count the hit; do not bake a silent nop. */
        d->op = q(VV_Q_RESERVED) ? OP_RNOP : OP_RI;
        return;
    }
    if (op >= 0x20 && op <= 0x2F) {
        d->op = OP_MEM;
        if (op == 0x27 || op == 0x2C || op == 0x2D || op == 0x2F) d->flags |= FL_ADDR0;
        if (op >= 0x28) d->flags |= FL_STORE;
        switch (op) {
        case 0x20: d->alu = 1; d->flags |= FL_SIGN; break;             /* lb */
        case 0x21: d->alu = 2; d->flags |= FL_SIGN; break;             /* lh */
        case 0x22: d->alu = 0; d->flags |= FL_LWL; break;
        case 0x23: d->alu = 4; break;                                  /* lw */
        case 0x24: d->alu = 1; break;                                  /* lbu */
        case 0x25: d->alu = 2; break;                                  /* lhu */
        case 0x26: d->alu = 0; d->flags |= FL_LWR; break;
        case 0x28: d->alu = 1; break;                                  /* sb */
        case 0x29: d->alu = 2; break;                                  /* sh */
        case 0x2A: d->alu = 0; d->flags |= FL_SWL; break;
        case 0x2B: d->alu = 4; break;                                  /* sw */
        case 0x2E: d->alu = 0; d->flags |= FL_SWR; break;
        default: d->alu = (d->flags & FL_ADDR0) ? 4 : 0; break;
        }
        return;
    }
    d->op = q(VV_Q_RESERVED) ? OP_RNOP : OP_RI;
}
