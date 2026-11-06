/* Predecoded basic-block L0 interpreter for --cpu=fast.
 * Quirk mask is baked at decode (default hardware set). Unicorn is not used for fetch/exec.
 * Execution is currently one IR at a time from the BB cache (decode once); block fusion
 * for fewer event checks comes after correctness gates. */
#include "cpu/fast.h"
#include "memmap.h"
#include "hw.h"
#include "quirk.h"
#include "dcache.h"
#include "term.h"
#include "display.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum {
    OP_NOP = 0, OP_BREAK, OP_SLOW,
    OP_ADDIU, OP_ADDI_WRAP, OP_ADDU, OP_ADD_WRAP, OP_SUBU, OP_SUB_WRAP,
    OP_ANDI, OP_ORI, OP_XORI, OP_LUI, OP_AND, OP_OR, OP_XOR, OP_NOR,
    OP_SLL, OP_SRL, OP_SRA, OP_SLLV, OP_SRLV, OP_SRAV,
    OP_SLT, OP_SLTU, OP_SLTI, OP_SLTIU,
    OP_LW, OP_LB, OP_LBU, OP_LH, OP_LHU, OP_SW, OP_SB, OP_SH,
    OP_BEQ, OP_BNE, OP_BLEZ, OP_BGTZ, OP_BLTZ, OP_BGEZ, OP_BLTZAL, OP_BGEZAL,
    OP_J, OP_JAL, OP_JR, OP_JALR,
    OP_MFC0, OP_MTC0,
    OP_MFHI0, OP_MFLO0, OP_SYSCALL_NOP, OP_MULT_NOP, OP_DIV_NOP,
};

typedef struct {
    uint16_t op;
    uint8_t  rd, rs, rt;
    int16_t  imm;
    uint32_t uimm;
    uint32_t pc;
} vv_ir;

#define BB_MAX 64
#define BB_CACHE 4096

typedef struct {
    uint32_t start_pc;
    uint32_t quirk_mask;
    int      n;
    vv_ir    ir[BB_MAX];
    uint32_t gen;
} vv_bb;

static vv_bb bb_cache[BB_CACHE];
static uint32_t bb_gen = 1;
static uint32_t decode_quirks;

static unsigned bb_slot(uint32_t pc) { return (pc >> 2) & (BB_CACHE - 1); }

void vv_fast_flush(void) { bb_gen++; if (!bb_gen) bb_gen = 1; }
void vv_fast_invalidate(uint32_t addr, uint32_t nbytes) {
    (void)addr; (void)nbytes;
    vv_fast_flush();
}

static uint32_t fetch_be(uint32_t pc, int *ok) {
    uint32_t c = vv_canon_inl(pc);
    if (c == 0xFFFFFFFFu || (c >> 28) == 0x8) { *ok = 0; return 0; }
    return vv_ram_be32(c, ok);
}

static int is_branch_op(uint16_t op) { return op >= OP_BEQ && op <= OP_JALR; }

static void decode_one(uint32_t pc, uint32_t insn, vv_ir *o) {
    unsigned op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, rd = (insn >> 11) & 31;
    unsigned sa = (insn >> 6) & 31, fn = insn & 63;
    int16_t imm = (int16_t)(insn & 0xFFFF);
    uint32_t uimm = insn & 0xFFFF;
    memset(o, 0, sizeof *o);
    o->pc = pc; o->rs = (uint8_t)rs; o->rt = (uint8_t)rt; o->rd = (uint8_t)rd;
    o->imm = imm; o->uimm = uimm;

    int wrap = vv_quirk_on(VV_Q_OVERFLOW);
    int hilo0 = vv_quirk_on(VV_Q_HILO);
    int sysnop = vv_quirk_on(VV_Q_SYSCALL);
    int resnop = vv_quirk_on(VV_Q_RESERVED);

    if (insn == 0) { o->op = OP_NOP; return; }

    switch (op) {
    case 0x00:
        switch (fn) {
        case 0x00: o->op = OP_SLL; o->uimm = sa; break;
        case 0x02: o->op = OP_SRL; o->uimm = sa; break;
        case 0x03: o->op = OP_SRA; o->uimm = sa; break;
        case 0x04: o->op = OP_SLLV; break;
        case 0x06: o->op = OP_SRLV; break;
        case 0x07: o->op = OP_SRAV; break;
        case 0x08: o->op = OP_JR; break;
        case 0x09: o->op = OP_JALR; if (!o->rd) o->rd = 31; break;
        case 0x0c: o->op = sysnop ? OP_SYSCALL_NOP : OP_SLOW; break;
        case 0x0d: o->op = OP_BREAK; break;
        case 0x10: o->op = hilo0 ? OP_MFHI0 : OP_SLOW; break;
        case 0x12: o->op = hilo0 ? OP_MFLO0 : OP_SLOW; break;
        case 0x11: case 0x13: o->op = hilo0 ? OP_NOP : OP_SLOW; break;
        case 0x18: case 0x19: case 0x1a: case 0x1b:
            o->op = hilo0 ? OP_MULT_NOP : OP_SLOW; break;
        case 0x20: o->op = wrap ? OP_ADD_WRAP : OP_SLOW; break;
        case 0x21: o->op = OP_ADDU; break;
        case 0x22: o->op = wrap ? OP_SUB_WRAP : OP_SLOW; break;
        case 0x23: o->op = OP_SUBU; break;
        case 0x24: o->op = OP_AND; break;
        case 0x25: o->op = OP_OR; break;
        case 0x26: o->op = OP_XOR; break;
        case 0x27: o->op = OP_NOR; break;
        case 0x2a: o->op = OP_SLT; break;
        case 0x2b: o->op = OP_SLTU; break;
        default:
            if (resnop) { o->op = OP_ADDU; o->rs = 0; o->rt = 0; } /* rd=0 */
            else o->op = OP_SLOW;
            break;
        }
        break;
    case 0x01:
        switch (rt) {
        case 0x00: o->op = OP_BLTZ; break;
        case 0x01: o->op = OP_BGEZ; break;
        case 0x10: o->op = OP_BLTZAL; break;
        case 0x11: o->op = OP_BGEZAL; break;
        default: o->op = resnop ? ((rt & 1) ? OP_BGEZ : OP_BLTZ) : OP_SLOW; break;
        }
        break;
    case 0x02: o->op = OP_J; o->uimm = insn & 0x03FFFFFF; break;
    case 0x03: o->op = OP_JAL; o->uimm = insn & 0x03FFFFFF; break;
    case 0x04: o->op = OP_BEQ; break;
    case 0x05: o->op = OP_BNE; break;
    case 0x06: o->op = OP_BLEZ; break;
    case 0x07: o->op = OP_BGTZ; break;
    case 0x08: o->op = wrap ? OP_ADDI_WRAP : OP_SLOW; break;
    case 0x09: o->op = OP_ADDIU; break;
    case 0x0a: o->op = OP_SLTI; break;
    case 0x0b: o->op = OP_SLTIU; break;
    case 0x0c: o->op = OP_ANDI; break;
    case 0x0d: o->op = OP_ORI; break;
    case 0x0e: o->op = OP_XORI; break;
    case 0x0f: o->op = OP_LUI; break;
    case 0x10:
        if ((rs == 0 || rs == 4) && (insn & 0x7FF) == 0) {
            o->op = (rs == 0) ? OP_MFC0 : OP_MTC0;
            o->rd = (uint8_t)((insn >> 11) & 31);
        } else o->op = resnop ? OP_NOP : OP_SLOW;
        break;
    case 0x20: o->op = OP_LB; break;
    case 0x21: o->op = OP_LH; break;
    case 0x23: o->op = OP_LW; break;
    case 0x24: o->op = OP_LBU; break;
    case 0x25: o->op = OP_LHU; break;
    case 0x28: o->op = OP_SB; break;
    case 0x29: o->op = OP_SH; break;
    case 0x2b: o->op = OP_SW; break;
    default: o->op = resnop ? OP_NOP : OP_SLOW; break;
    }
}

static vv_bb *get_bb(uint32_t pc) {
    unsigned s = bb_slot(pc);
    vv_bb *b = &bb_cache[s];
    if (b->gen == bb_gen && b->start_pc == pc && b->quirk_mask == decode_quirks) return b;

    b->start_pc = pc;
    b->quirk_mask = decode_quirks;
    b->gen = bb_gen;
    b->n = 0;
    uint32_t cur = pc;
    for (int i = 0; i < BB_MAX; i++) {
        int ok = 1;
        uint32_t insn = fetch_be(cur, &ok);
        if (!ok) {
            if (vv_quirk_on(VV_Q_UNMAPPED_FETCH)) insn = 0;
            else { decode_one(cur, 0, &b->ir[i]); b->ir[i].op = OP_SLOW; b->n = i + 1; break; }
        }
        decode_one(cur, insn, &b->ir[i]);
        b->n = i + 1;
        if (b->ir[i].op == OP_BREAK || b->ir[i].op == OP_SLOW || is_branch_op(b->ir[i].op))
            break;
        if (((cur + 4) & ~0xFFFu) != (cur & ~0xFFFu)) break;
        cur += 4;
    }
    return b;
}

static const vv_ir *lookup_ir(uint32_t pc) {
    vv_bb *b = get_bb(pc);
    for (int i = 0; i < b->n; i++) if (b->ir[i].pc == pc) return &b->ir[i];
    /* delay slot or mid-block miss: decode once into a thread-local scratch via static */
    static vv_ir scratch;
    int ok = 1;
    uint32_t insn = fetch_be(pc, &ok);
    if (!ok) insn = vv_quirk_on(VV_Q_UNMAPPED_FETCH) ? 0 : 0;
    decode_one(pc, insn, &scratch);
    if (!ok && !vv_quirk_on(VV_Q_UNMAPPED_FETCH)) scratch.op = OP_SLOW;
    return &scratch;
}

static uint32_t gr(vv_fast_cpu *c, unsigned r) {
    if (!r) return 0;
    if (c->load_rt >= 0 && (int)r == c->load_rt && vv_quirk_on(VV_Q_LOAD_USE))
        return c->load_old;
    return c->gpr[r];
}
static void sr(vv_fast_cpu *c, unsigned r, uint32_t v) { if (r) c->gpr[r] = v; }

static uint32_t mem_load(vv_fast_env *e, uint32_t va, unsigned size, int signed_ext, int *fault) {
    *fault = 0;
    uint32_t c = vv_canon_inl(va);
    if (c == 0xFFFFFFFFu) {
        if (vv_quirk_on(VV_Q_UNMAPPED)) return 0;
        *fault = 1; return 0;
    }
    if ((c >> 28) == 0x8) {
        uint64_t v = e->mmio_read(e->uc, (c & ~3u) - VV_MMIO_BASE + (va & 3), size, NULL);
        if (size < 4 && signed_ext) {
            uint32_t sh = 32 - 8 * size;
            return (uint32_t)((int32_t)((uint32_t)v << sh) >> sh);
        }
        return (uint32_t)v;
    }
    if ((size == 4 && (va & 3)) || (size == 2 && (va & 1))) {
        if (vv_quirk_on(VV_Q_UNALIGNED)) return 0;
        *fault = 1; return 0;
    }
    uint8_t *p = vv_ram(c, size);
    if (!p) { if (vv_quirk_on(VV_Q_UNMAPPED)) return 0; *fault = 1; return 0; }
    uint32_t v = 0;
    if (e->guest_big) for (unsigned i = 0; i < size; i++) v = (v << 8) | p[i];
    else for (unsigned i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    if (signed_ext && size < 4) {
        uint32_t sh = 32 - 8 * size;
        v = (uint32_t)((int32_t)(v << sh) >> sh);
    }
    if (size == 4 && ((c >> 28) == 0x1 || (c >> 28) == 0x3)) {
        uint32_t stale; (void)vv_dc_load(c, &stale);
    }
    return v;
}

static void mem_store(vv_fast_env *e, vv_fast_cpu *cpu, uint32_t va, unsigned size, uint32_t val, int *fault) {
    *fault = 0;
    uint32_t c = vv_canon_inl(va);
    if (c == 0xFFFFFFFFu) {
        if (vv_quirk_on(VV_Q_UNMAPPED)) return;
        *fault = 1; return;
    }
    if ((c >> 28) == 0x4 && vv_quirk_on(VV_Q_BIOS_STORE)) return;
    if ((va >> 28) == 0x2 && vv_quirk_on(VV_Q_DDR2)) {
        /* Hardware drops unless PC is in BIOS. */
        if ((cpu->pc >> 28) != 0x4) return;
    }
    if ((c >> 28) == 0x8) {
        e->mmio_write(e->uc, (c & ~3u) - VV_MMIO_BASE + (va & 3), size, val, NULL);
        return;
    }
    if ((size == 4 && (va & 3)) || (size == 2 && (va & 1))) {
        if (vv_quirk_on(VV_Q_UNALIGNED)) return;
        *fault = 1; return;
    }
    uint8_t *p = vv_ram(c, size);
    if (!p) { if (vv_quirk_on(VV_Q_UNMAPPED)) return; *fault = 1; return; }
    if (e->guest_big) for (unsigned i = 0; i < size; i++) p[i] = (uint8_t)(val >> (8 * (size - 1 - i)));
    else for (unsigned i = 0; i < size; i++) p[i] = (uint8_t)(val >> (8 * i));
    if ((c >> 28) == 0x1 || (c >> 28) == 0x3) vv_dc_store(c, size, val);
    vv_fast_invalidate(c, size);
}

static int exec_one(vv_fast_env *e, vv_fast_cpu *c, const vv_ir *o) {
    int fault = 0;
    uint32_t rs = gr(c, o->rs), rt = gr(c, o->rt);
    int32_t rsi = (int32_t)rs, rti = (int32_t)rt;
    uint32_t link = o->pc + 8;
    int next_load_rt = -1;
    uint32_t next_load_old = 0;
    int taken = 0;
    uint32_t target = 0;

    switch (o->op) {
    case OP_NOP: case OP_SYSCALL_NOP: case OP_MULT_NOP: case OP_DIV_NOP: break;
    case OP_BREAK: c->stop_break = 1; break;
    case OP_SLOW:
        vv_eprintf("[fast] unsupported insn at pc=0x%08x\n", o->pc);
        return -1;
    case OP_ADDIU: case OP_ADDI_WRAP: sr(c, o->rt, rs + (uint32_t)(int32_t)o->imm); break;
    case OP_ADDU: case OP_ADD_WRAP: sr(c, o->rd, rs + rt); break;
    case OP_SUBU: case OP_SUB_WRAP: sr(c, o->rd, rs - rt); break;
    case OP_ANDI: sr(c, o->rt, rs & o->uimm); break;
    case OP_ORI:  sr(c, o->rt, rs | o->uimm); break;
    case OP_XORI: sr(c, o->rt, rs ^ o->uimm); break;
    case OP_LUI:  sr(c, o->rt, o->uimm << 16); break;
    case OP_AND:  sr(c, o->rd, rs & rt); break;
    case OP_OR:   sr(c, o->rd, rs | rt); break;
    case OP_XOR:  sr(c, o->rd, rs ^ rt); break;
    case OP_NOR:  sr(c, o->rd, ~(rs | rt)); break;
    case OP_SLL:  sr(c, o->rd, rt << (o->uimm & 31)); break;
    case OP_SRL:  sr(c, o->rd, rt >> (o->uimm & 31)); break;
    case OP_SRA:  sr(c, o->rd, (uint32_t)(rti >> (o->uimm & 31))); break;
    case OP_SLLV: sr(c, o->rd, rt << (rs & 31)); break;
    case OP_SRLV: sr(c, o->rd, rt >> (rs & 31)); break;
    case OP_SRAV: sr(c, o->rd, (uint32_t)(rti >> (rs & 31))); break;
    case OP_SLT:  sr(c, o->rd, rsi < rti); break;
    case OP_SLTU: sr(c, o->rd, rs < rt); break;
    case OP_SLTI: sr(c, o->rt, rsi < (int32_t)o->imm); break;
    case OP_SLTIU: sr(c, o->rt, rs < (uint32_t)(int32_t)o->imm); break;
    case OP_MFHI0: case OP_MFLO0: sr(c, o->rd, 0); break;
    case OP_LW: case OP_LB: case OP_LBU: case OP_LH: case OP_LHU: {
        unsigned sz = (o->op == OP_LW) ? 4 : (o->op == OP_LH || o->op == OP_LHU) ? 2 : 1;
        int sex = (o->op == OP_LB || o->op == OP_LH);
        if (!vv_quirk_on(VV_Q_LB_SIGN) && (o->op == OP_LB || o->op == OP_LH)) sex = 1;
        if (vv_quirk_on(VV_Q_LB_SIGN) && (o->op == OP_LB || o->op == OP_LH)) sex = 0; /* old HW zero-ext */
        uint32_t addr = rs + (uint32_t)(int32_t)o->imm;
        uint32_t old = c->gpr[o->rt];
        uint32_t v = mem_load(e, addr, sz, sex, &fault);
        if (fault) return -1;
        if (o->rt) {
            if (vv_quirk_on(VV_Q_LOAD_USE)) { next_load_rt = o->rt; next_load_old = old; }
            c->gpr[o->rt] = v;
        }
        break;
    }
    case OP_SW: case OP_SB: case OP_SH: {
        unsigned sz = (o->op == OP_SW) ? 4 : (o->op == OP_SH) ? 2 : 1;
        mem_store(e, c, rs + (uint32_t)(int32_t)o->imm, sz, rt, &fault);
        if (fault) return -1;
        break;
    }
    case OP_BEQ:  taken = rs == rt; target = o->pc + 4 + ((int32_t)o->imm << 2); break;
    case OP_BNE:  taken = rs != rt; target = o->pc + 4 + ((int32_t)o->imm << 2); break;
    case OP_BLEZ: taken = rsi <= 0; target = o->pc + 4 + ((int32_t)o->imm << 2); break;
    case OP_BGTZ: taken = rsi > 0;  target = o->pc + 4 + ((int32_t)o->imm << 2); break;
    case OP_BLTZ: taken = rsi < 0;  target = o->pc + 4 + ((int32_t)o->imm << 2); break;
    case OP_BGEZ: taken = rsi >= 0; target = o->pc + 4 + ((int32_t)o->imm << 2); break;
    case OP_BLTZAL:
        taken = rsi < 0; target = o->pc + 4 + ((int32_t)o->imm << 2);
        if (taken || !vv_quirk_on(VV_Q_BAL_UNTAKEN)) sr(c, 31, link);
        break;
    case OP_BGEZAL:
        taken = rsi >= 0; target = o->pc + 4 + ((int32_t)o->imm << 2);
        if (taken || !vv_quirk_on(VV_Q_BAL_UNTAKEN)) sr(c, 31, link);
        break;
    case OP_J:
        taken = 1; target = (o->pc & 0xF0000000u) | (o->uimm << 2); break;
    case OP_JAL:
        sr(c, 31, link);
        taken = 1; target = (o->pc & 0xF0000000u) | (o->uimm << 2); break;
    case OP_JR:
        taken = 1; target = rs; break;
    case OP_JALR:
        sr(c, o->rd, link);
        taken = 1; target = rs; break;
    case OP_MFC0: {
        uint32_t v = vv_cop0_read(o->rd);
        if (!vv_quirk_on(VV_Q_COP0)) {
            if (o->rd == 12) v = vv_hw_status();
            else if (o->rd == 13) v = vv_hw_cause();
            else if (o->rd == 14) v = vv_hw_epc();
        }
        sr(c, o->rt, v);
        break;
    }
    case OP_MTC0:
        if (vv_quirk_on(VV_Q_COP0)) vv_cop0_write(o->rd, rt);
        else {
            if (o->rd == 12) vv_hw_set_status(rt);
            else if (o->rd == 14) vv_hw_set_epc(rt);
            else vv_cop0_write(o->rd, rt);
        }
        break;
    default: return -1;
    }

    if (is_branch_op(o->op)) {
        c->in_delay = 1;
        c->delay_npc = taken ? target : (o->pc + 8);
    }

    c->load_rt = next_load_rt;
    c->load_old = next_load_old;
    return 0;
}

static int is_branch_word(uint32_t i) {
    unsigned op = i >> 26, rt = (i >> 16) & 31, fn = i & 63;
    if (op == 0 && (fn == 8 || fn == 9)) return 1;
    if (op == 1 && (rt == 0 || rt == 1 || rt == 16 || rt == 17)) return 1;
    return op >= 2 && op <= 7;
}
static int is_mtc0_word(uint32_t i) { return (i >> 21) == 0x204; }

static void irq_step_fast(vv_fast_env *e, vv_fast_cpu *c) {
    static uint32_t prev_status;
    if (!e->opt_irq) {
        c->last3_pc = c->last2_pc; c->last2_pc = c->last_pc; c->last_pc = c->pc;
        return;
    }
    if (!vv_hw_irq_possible()) {
        if (vv_quirk_halted) return;
        c->last3_pc = c->last2_pc; c->last2_pc = c->last_pc; c->last_pc = c->pc;
        return;
    }
    uint32_t st = vv_hw_status();
    if ((st & 1) && !(prev_status & 1)) vv_hw_isr_exit_check(st, c->pc, "IE re-enabled");
    prev_status = st;
    if (vv_quirk_halted) return;
    if (vv_hw_irq_request(st)) {
        int defer = 0;
        if (c->last_pc != 0xFFFFFFFFu) {
            int ok; uint32_t x = fetch_be(c->last_pc, &ok);
            if (ok) defer = is_branch_word(x) || is_mtc0_word(x);
        }
        if (!defer && c->last3_pc != 0xFFFFFFFFu) {
            int ok; uint32_t x = fetch_be(c->last3_pc, &ok);
            if (ok && is_branch_word(x) && c->last_pc != c->last3_pc + 8) defer = 1;
        }
        if (!defer) {
            VV_EVT("irq-enter cause 0x%04x status 0x%08x", vv_hw_cause() & 0xFFFFu, st);
            vv_hw_isr_enter(c->pc, st); st &= ~1u; prev_status = st;
            c->pc = VV_PC_ISR;
            c->in_delay = 0;
            c->load_rt = -1;
            c->last_pc = c->last2_pc = c->last3_pc = 0xFFFFFFFFu;
            return;
        }
    }
    c->last3_pc = c->last2_pc; c->last2_pc = c->last_pc; c->last_pc = c->pc;
}

void vv_fast_sync_to_uc(uc_engine *uc, const vv_fast_cpu *cpu) {
    for (int i = 1; i < 32; i++) {
        uint32_t v = cpu->gpr[i];
        uc_reg_write(uc, UC_MIPS_REG_0 + i, &v);
    }
    uint32_t pc = cpu->pc;
    uc_reg_write(uc, UC_MIPS_REG_PC, &pc);
    uc_reg_write(uc, UC_MIPS_REG_HI, (void *)&cpu->hi);
    uc_reg_write(uc, UC_MIPS_REG_LO, (void *)&cpu->lo);
}

int vv_fast_run(vv_fast_env *env, vv_fast_cpu *cpu, uint32_t entry) {
    decode_quirks = vv_quirk_emul;
    vv_fast_flush();
    memset(cpu->gpr, 0, sizeof cpu->gpr);
    cpu->pc = entry;
    cpu->hi = cpu->lo = 0;
    cpu->load_rt = -1;
    cpu->in_delay = 0;
    cpu->last_pc = cpu->last2_pc = cpu->last3_pc = 0xFFFFFFFFu;
    cpu->stop_break = cpu->stop_until = 0;

    /* Preserve caller-preset SP if Unicorn already has it (main wrote --sp before run). */
    {
        uint32_t sp = 0;
        uc_reg_read(env->uc, UC_MIPS_REG_SP, &sp);
        if (sp) cpu->gpr[29] = sp;
    }

    while (!cpu->stop_break && !cpu->stop_until) {
        if (env->stop_req && *env->stop_req) break;
        if (env->max_insns) {
            if (env->hook_calls) ++*env->hook_calls;
            if (env->hook_calls && *env->hook_calls > env->max_insns) {
                if (env->stopped_max) *env->stopped_max = 1;
                break;
            }
        }
        if (env->have_until && cpu->pc == env->until_addr) { cpu->stop_until = 1; break; }

        uint32_t pc = cpu->pc;
        vv_cur_pc = pc;
        if (pc - 0x10000000u < 0x30000000u && env->note_ddr_exec) env->note_ddr_exec(pc);

        (*env->insn_count)++;
        vv_hw_tick(env->uc, *env->insn_count);
        if (vv_quirk_halted) break;
        if (env->nevents && env->run_due_events) env->run_due_events(env->uc, 0);

        /* IRQ before executing this PC (matches hook_code / irq_step order). */
        uint32_t pc_before_irq = cpu->pc;
        irq_step_fast(env, cpu);
        if (vv_quirk_halted) break;
        if (cpu->pc != pc_before_irq) continue; /* redirected to ISR */

        const vv_ir *o = lookup_ir(cpu->pc);
        if (env->opt_trace && env->trace_left && *env->trace_left != 0) {
            int ok; uint32_t insn = fetch_be(o->pc, &ok);
            vv_eprintf("[trace] %8llu pc=0x%08x insn=%08x\n",
                       (unsigned long long)*env->insn_count, o->pc, ok ? insn : 0);
            if (*env->trace_left > 0) (*env->trace_left)--;
        }

        int was_delay = cpu->in_delay;
        uint32_t delay_npc = cpu->delay_npc;
        /* Clear in_delay before exec so a branch in the delay slot can set it again (rare). */
        if (was_delay) cpu->in_delay = 0;

        int rc = exec_one(env, cpu, o);
        if (rc < 0) return 1;
        if (cpu->stop_break) { cpu->pc = o->pc + 4; break; }

        if (was_delay) {
            cpu->pc = delay_npc;
        } else if (cpu->in_delay) {
            /* Just took a branch: next PC is the delay slot. */
            cpu->pc = o->pc + 4;
        } else {
            cpu->pc = o->pc + 4;
        }

        if ((*env->insn_count & 0xFFF) == 0) {
            if (env->max_wall_ns && vv_mono_ns() - env->run_t0 >= env->max_wall_ns) {
                if (env->stopped_wall) *env->stopped_wall = 1;
                if (env->stop_req) *env->stop_req = 1;
                break;
            }
            vv_term_tick();
            int why = vv_disp_tick(env->uc);
            if (why) {
                vv_eprintf(why == VV_DISP_ESC ? "[stop] Esc pressed\n" : "[stop] window closed\n");
                if (env->stop_req) *env->stop_req = 1;
                break;
            }
        }
    }
    return 0;
}
