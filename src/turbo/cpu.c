/* Fixed MIPS150 direct-threaded, decode-once interpreter. No runtime code generation.
 * Static branch edges are chained; indirect jumps always perform a PC cache lookup. */
#include "turbo.h"
#include "hw.h"
#include "term.h"
#include <stdlib.h>
#include <limits.h>
#define BB_SIZE 32
#define BB_COUNT 4096
enum { O_NOP, O_ZERO, O_BREAK, O_ADDI, O_ADD, O_SUB, O_ANDI, O_ORI, O_XORI, O_LUI, O_AND, O_OR, O_XOR, O_NOR, O_SLL, O_SRL, O_SRA, O_SLLV, O_SRLV, O_SRAV, O_SLT, O_SLTU, O_SLTI, O_SLTIU, O_LW, O_LB, O_LBU, O_LH, O_LHU, O_SW, O_SB, O_SH, O_LOADZERO, O_ADDRZERO, O_BEQ, O_BNE, O_BLEZ, O_BGTZ, O_BLTZ, O_BGEZ, O_BLTZAL, O_BGEZAL, O_J, O_JAL, O_JR, O_JALR, O_MFC0, O_MTC0 };
typedef struct {
    void *handler;
    uint32_t pc, target, uimm;
    int16_t imm;
    uint8_t rs, rt, rd, op, last;
} ir;
typedef struct block {
    uint32_t pc; unsigned gen, n;
    struct block *edge[2];
    uint32_t edge_pc[2];
    ir code[BB_SIZE];
} block;

static void decode(ir *o, uint32_t pc, uint32_t w, void **labels) {
    memset(o, 0, sizeof *o);
    unsigned op = w >> 26, fn = w & 63, sa = (w >> 6) & 31;
    o->rs = (w >> 21) & 31; o->rt = (w >> 16) & 31; o->rd = (w >> 11) & 31;
    o->pc = pc; o->imm = (int16_t)w; o->uimm = w & 65535;
    o->target = pc + 4 + (uint32_t)((int32_t)o->imm * 4);
    o->op = O_NOP;
    switch (op) {
    case 0:
        switch (fn) {
        case 0: o->op = O_SLL; o->uimm = sa; break;
        case 2: o->op = O_SRL; o->uimm = sa; break;
        case 3: o->op = O_SRA; o->uimm = sa; break;
        case 4: o->op = O_SLLV; break;
        case 6: o->op = O_SRLV; break;
        case 7: o->op = O_SRAV; break;
        case 8: o->op = O_JR; break;
        case 9: o->op = O_JALR; break;
        case 12: o->op = O_NOP; break;
        case 13: o->op = O_BREAK; break;
        case 16: o->op = O_ZERO; break;
        case 18: o->op = O_ZERO; break;
        case 17: o->op = O_NOP; break;
        case 19: o->op = O_NOP; break;
        case 24: o->op = O_NOP; break;
        case 25: o->op = O_NOP; break;
        case 26: o->op = O_NOP; break;
        case 27: o->op = O_NOP; break;
        case 32: o->op = O_ADD; break;
        case 33: o->op = O_ADD; break;
        case 34: o->op = O_SUB; break;
        case 35: o->op = O_SUB; break;
        case 36: o->op = O_AND; break;
        case 37: o->op = O_OR; break;
        case 38: o->op = O_XOR; break;
        case 39: o->op = O_NOR; break;
        case 42: o->op = O_SLT; break;
        case 43: o->op = O_SLTU; break;
        default: o->op = O_ZERO; break;
        } break;
    case 1:
        o->op = (o->rt & 1) ? ((o->rt & 16) ? O_BGEZAL : O_BGEZ) : ((o->rt & 16) ? O_BLTZAL : O_BLTZ); break;
    case 2: case 3:
        o->op = op == 2 ? O_J : O_JAL;
        o->target = ((pc + 4) & 0xf0000000u) | ((w & 0x3ffffff) << 2); break;
    case 4: o->op = O_BEQ; break;
    case 5: o->op = O_BNE; break;
    case 6: o->op = O_BLEZ; break;
    case 7: o->op = O_BGTZ; break;
    case 8: o->op = O_ADDI; break;
    case 9: o->op = O_ADDI; break;
    case 10: o->op = O_SLTI; break;
    case 11: o->op = O_SLTIU; break;
    case 12: o->op = O_ANDI; break;
    case 13: o->op = O_ORI; break;
    case 14: o->op = O_XORI; break;
    case 15: o->op = O_LUI; break;
    case 32: o->op = O_LB; break;
    case 33: o->op = O_LH; break;
    case 34: o->op = O_LOADZERO; break;
    case 35: o->op = O_LW; break;
    case 36: o->op = O_LBU; break;
    case 37: o->op = O_LHU; break;
    case 38: o->op = O_LOADZERO; break;
    case 39: o->op = O_ADDRZERO; break;
    case 40: o->op = O_SB; break;
    case 41: o->op = O_SH; break;
    case 43: o->op = O_SW; break;
    case 16:
        if ((o->rs == 0 || o->rs == 4) && !(w & 0x7f8)) o->op = o->rs == 0 ? O_MFC0 : O_MTC0;
        break;
    }
    o->handler = labels[o->op];
}
static int branch(unsigned op) { return op >= O_BEQ && op <= O_JALR; }
static block *lookup(turbo *t, uint32_t pc, unsigned gen, void **labels) {
    unsigned hash = ((pc >> 2) ^ (pc >> 14) ^ (pc >> 24)) & (BB_COUNT - 1);
    block *b = &((block *)t->cache)[hash];
    if (b->gen == gen && b->pc == pc && b->n) return b;
    memset(b, 0, sizeof *b); b->pc = pc; b->gen = gen;
    int slot = 0;
    for (unsigned i = 0; i < BB_SIZE; i++, pc += 4) {
        uint8_t *p = turbo_ptr(t, pc & ~3u, 4);
        if (p) turbo_mark(t, pc);
        decode(&b->code[i], pc, p ? turbo_word(p) : 0, labels);
        b->n = i + 1;
        if (slot || b->code[i].op == O_BREAK) break;
        if (branch(b->code[i].op)) {
            /* Never split branch+slot across blocks. */
            if (i == BB_SIZE - 1) { b->n--; break; }
            slot = 1;
        }
        if (!slot && ((pc + 4) & 4095) == 0) break;
    }
    b->code[b->n - 1].last = 1;
    return b;
}

void turbo_cpu(turbo *t) {
    void *labels[] = {&&L_NOP, &&L_ZERO, &&L_BREAK, &&L_ADDI, &&L_ADD, &&L_SUB, &&L_ANDI, &&L_ORI, &&L_XORI, &&L_LUI, &&L_AND, &&L_OR, &&L_XOR, &&L_NOR, &&L_SLL, &&L_SRL, &&L_SRA, &&L_SLLV, &&L_SRLV, &&L_SRAV, &&L_SLT, &&L_SLTU, &&L_SLTI, &&L_SLTIU, &&L_LW, &&L_LB, &&L_LBU, &&L_LH, &&L_LHU, &&L_SW, &&L_SB, &&L_SH, &&L_LOADZERO, &&L_ADDRZERO, &&L_BEQ, &&L_BNE, &&L_BLEZ, &&L_BGTZ, &&L_BLTZ, &&L_BGEZ, &&L_BLTZAL, &&L_BGEZAL, &&L_J, &&L_JAL, &&L_JR, &&L_JALR, &&L_MFC0, &&L_MTC0};
    t->cache = calloc(BB_COUNT, sizeof(block));
    if (!t->cache) { vv_eprintf("[turbo] cannot allocate decode cache\n"); t->error = t->stopped = 1; return; }
    block *b = NULL, *prev = NULL;
    ir *o;
    uint32_t rs, rt, target = 0;
    unsigned n = 0, gen = 0, budget = 0;
    int edge = 2, prev_edge = 2, force_exit = 0, has_target = 0;
#define WRITE(r,v) do { unsigned reg_ = (r); if (reg_) t->gpr[reg_] = (v); } while (0)
#define CLOCK() do { t->count += n; t->insns += n; n = 0; } while (0)
#define DISPATCH() do { \
    t->pc = o->pc; \
    if (!budget-- || t->stopped) goto stop; \
    n++; \
    rs = (int)o->rs == t->load_rt ? t->load_old : t->gpr[o->rs]; \
    rt = (int)o->rt == t->load_rt ? t->load_old : t->gpr[o->rt]; \
    t->load_rt = -1; \
    goto *o->handler; \
} while (0)
#define NEXT() do { \
    if (o->last || force_exit || t->stopped) goto block_end; \
    ++o; DISPATCH(); \
} while (0)
block_start:
    if (atomic_load_explicit(&t->stop, memory_order_relaxed) || t->stopped || (t->max_insns && t->insns >= t->max_insns)) goto stop;
    if (t->insns >= t->event_at) turbo_events(t);
    if ((t->status & 1) && (t->status & t->cause & 0xfc00)) {
        t->epc = t->pc; t->status &= ~1u; t->pc = VV_PC_ISR; t->irqs++; t->load_rt = -1; prev = NULL;
    }
    gen = atomic_load_explicit(&t->epoch, memory_order_acquire);
    if (prev && prev_edge < 2 && prev->gen == gen && prev->edge[prev_edge] && prev->edge_pc[prev_edge] == t->pc &&
        prev->edge[prev_edge]->gen == gen && prev->edge[prev_edge]->pc == t->pc) b = prev->edge[prev_edge];
    else {
        uint32_t from = prev ? prev->pc : 0;
        b = lookup(t, t->pc, gen, labels);
        if (prev && prev_edge < 2 && prev->pc == from && prev->gen == gen && prev != b) {
            prev->edge[prev_edge] = b; prev->edge_pc[prev_edge] = t->pc;
        }
    }
    o = b->code; target = 0; edge = 0; force_exit = 0; has_target = 0;
    budget = b->n;
    if (t->max_insns && t->max_insns - t->insns < budget) budget = (unsigned)(t->max_insns - t->insns);
    DISPATCH();
L_NOP: {
        
        NEXT();
    }
L_ZERO: {
        WRITE(o->rd, 0);
        NEXT();
    }
L_BREAK: {
        t->stopped = 1;
        NEXT();
    }
L_ADDI: {
        WRITE(o->rt, rs + (uint32_t)(int32_t)o->imm);
        NEXT();
    }
L_ADD: {
        WRITE(o->rd, rs + rt);
        NEXT();
    }
L_SUB: {
        WRITE(o->rd, rs - rt);
        NEXT();
    }
L_ANDI: {
        WRITE(o->rt, rs & o->uimm);
        NEXT();
    }
L_ORI: {
        WRITE(o->rt, rs | o->uimm);
        NEXT();
    }
L_XORI: {
        WRITE(o->rt, rs ^ o->uimm);
        NEXT();
    }
L_LUI: {
        WRITE(o->rt, o->uimm << 16);
        NEXT();
    }
L_AND: {
        WRITE(o->rd, rs & rt);
        NEXT();
    }
L_OR: {
        WRITE(o->rd, rs | rt);
        NEXT();
    }
L_XOR: {
        WRITE(o->rd, rs ^ rt);
        NEXT();
    }
L_NOR: {
        WRITE(o->rd, ~(rs | rt));
        NEXT();
    }
L_SLL: {
        WRITE(o->rd, rt << o->uimm);
        NEXT();
    }
L_SRL: {
        WRITE(o->rd, rt >> o->uimm);
        NEXT();
    }
L_SRA: {
        WRITE(o->rd, (uint32_t)((int32_t)rt >> o->uimm));
        NEXT();
    }
L_SLLV: {
        WRITE(o->rd, rt << (rs & 31));
        NEXT();
    }
L_SRLV: {
        WRITE(o->rd, rt >> (rs & 31));
        NEXT();
    }
L_SRAV: {
        WRITE(o->rd, (uint32_t)((int32_t)rt >> (rs & 31)));
        NEXT();
    }
L_SLT: {
        WRITE(o->rd, (int32_t)rs < (int32_t)rt);
        NEXT();
    }
L_SLTU: {
        WRITE(o->rd, rs < rt);
        NEXT();
    }
L_SLTI: {
        WRITE(o->rt, (int32_t)rs < (int32_t)o->imm);
        NEXT();
    }
L_SLTIU: {
        WRITE(o->rt, rs < (uint32_t)(int32_t)o->imm);
        NEXT();
    }
L_LW: {
        uint32_t a = rs + (uint32_t)(int32_t)o->imm;
        if ((a >> 28) == 8) { CLOCK(); turbo_events(t); }
        uint32_t old = t->gpr[o->rt];
        uint32_t v = turbo_load_mem(t, a, 4, 0);
        if (o->rt) { t->load_rt = o->rt; t->load_old = old; t->gpr[o->rt] = v; }
        NEXT();
    }
L_LB: {
        uint32_t a = rs + (uint32_t)(int32_t)o->imm;
        if ((a >> 28) == 8) { CLOCK(); turbo_events(t); }
        uint32_t old = t->gpr[o->rt];
        uint32_t v = turbo_load_mem(t, a, 1, 1);
        if (o->rt) { t->load_rt = o->rt; t->load_old = old; t->gpr[o->rt] = v; }
        NEXT();
    }
L_LBU: {
        uint32_t a = rs + (uint32_t)(int32_t)o->imm;
        if ((a >> 28) == 8) { CLOCK(); turbo_events(t); }
        uint32_t old = t->gpr[o->rt];
        uint32_t v = turbo_load_mem(t, a, 1, 0);
        if (o->rt) { t->load_rt = o->rt; t->load_old = old; t->gpr[o->rt] = v; }
        NEXT();
    }
L_LH: {
        uint32_t a = rs + (uint32_t)(int32_t)o->imm;
        if ((a >> 28) == 8) { CLOCK(); turbo_events(t); }
        uint32_t old = t->gpr[o->rt];
        uint32_t v = turbo_load_mem(t, a, 2, 1);
        if (o->rt) { t->load_rt = o->rt; t->load_old = old; t->gpr[o->rt] = v; }
        NEXT();
    }
L_LHU: {
        uint32_t a = rs + (uint32_t)(int32_t)o->imm;
        if ((a >> 28) == 8) { CLOCK(); turbo_events(t); }
        uint32_t old = t->gpr[o->rt];
        uint32_t v = turbo_load_mem(t, a, 2, 0);
        if (o->rt) { t->load_rt = o->rt; t->load_old = old; t->gpr[o->rt] = v; }
        NEXT();
    }
L_SW: {
        uint32_t a = rs + (uint32_t)(int32_t)o->imm;
        if ((a >> 28) == 8) { CLOCK(); turbo_events(t); }
        turbo_store_mem(t, a, 4, rt);
        if (atomic_load_explicit(&t->epoch, memory_order_acquire) != gen) force_exit = 1;
        NEXT();
    }
L_SB: {
        uint32_t a = rs + (uint32_t)(int32_t)o->imm;
        if ((a >> 28) == 8) { CLOCK(); turbo_events(t); }
        turbo_store_mem(t, a, 1, rt);
        if (atomic_load_explicit(&t->epoch, memory_order_acquire) != gen) force_exit = 1;
        NEXT();
    }
L_SH: {
        uint32_t a = rs + (uint32_t)(int32_t)o->imm;
        if ((a >> 28) == 8) { CLOCK(); turbo_events(t); }
        turbo_store_mem(t, a, 2, rt);
        if (atomic_load_explicit(&t->epoch, memory_order_acquire) != gen) force_exit = 1;
        NEXT();
    }
L_LOADZERO: {
        uint32_t a = rs + (uint32_t)(int32_t)o->imm;
        if ((a >> 28) == 8) { CLOCK(); turbo_events(t); (void)turbo_mmio_read(t, a); }
        if (o->rt) { t->load_rt = o->rt; t->load_old = t->gpr[o->rt]; t->gpr[o->rt] = 0; }
        NEXT();
    }
L_ADDRZERO: {
        if (o->rt) { t->load_rt = o->rt; t->load_old = t->gpr[o->rt]; t->gpr[o->rt] = 0; }
        NEXT();
    }
L_BEQ: {
        has_target = 1;
        int taken = rs == rt; target = taken ? o->target : o->pc + 8; edge = taken;
        NEXT();
    }
L_BNE: {
        has_target = 1;
        int taken = rs != rt; target = taken ? o->target : o->pc + 8; edge = taken;
        NEXT();
    }
L_BLEZ: {
        has_target = 1;
        int taken = (int32_t)rs <= 0; target = taken ? o->target : o->pc + 8; edge = taken;
        NEXT();
    }
L_BGTZ: {
        has_target = 1;
        int taken = (int32_t)rs > 0; target = taken ? o->target : o->pc + 8; edge = taken;
        NEXT();
    }
L_BLTZ: {
        has_target = 1;
        int taken = (int32_t)rs < 0; target = taken ? o->target : o->pc + 8; edge = taken;
        NEXT();
    }
L_BGEZ: {
        has_target = 1;
        int taken = (int32_t)rs >= 0; target = taken ? o->target : o->pc + 8; edge = taken;
        NEXT();
    }
L_BLTZAL: {
        has_target = 1;
        int taken = (int32_t)rs < 0; target = taken ? o->target : o->pc + 8; edge = taken; if (taken) WRITE(31, o->pc + 8);
        NEXT();
    }
L_BGEZAL: {
        has_target = 1;
        int taken = (int32_t)rs >= 0; target = taken ? o->target : o->pc + 8; edge = taken; if (taken) WRITE(31, o->pc + 8);
        NEXT();
    }
L_J: {
        has_target = 1;
        target = o->target; edge = 1;
        NEXT();
    }
L_JAL: {
        has_target = 1;
        WRITE(31, o->pc + 8); target = o->target; edge = 1;
        NEXT();
    }
L_JR: {
        has_target = 1;
        target = rs; edge = 2;
        NEXT();
    }
L_JALR: {
        has_target = 1;
        WRITE(o->rd, o->pc + 8); target = rs; edge = 2;
        NEXT();
    }
L_MFC0: {
        CLOCK(); turbo_events(t);
        uint32_t v = o->rd == 9 ? t->count : o->rd == 11 ? t->compare : o->rd == 12 ? t->status : o->rd == 13 ? t->cause : o->rd == 14 ? t->epc : 0;
        WRITE(o->rt, v);
        NEXT();
    }
L_MTC0: {
        CLOCK(); turbo_events(t);
        switch (o->rd) {
        case 9: t->count = rt - 1; t->event_count = t->count; break; /* Count write takes effect on the next cycle */
        case 11: t->compare = rt; t->cause &= ~0x8000u; break;
        case 12: t->status = rt; break;
        case 13: t->cause = (rt & ~0xfc00u) | (t->cause & rt & 0xfc00u); break;
        }
        t->event_at = t->insns + 1;
        NEXT();
    }
block_end:
    CLOCK();
    /* A branch and its decoded delay instruction retire together. No in-delay
     * state on ALU handlers and no second lookup for the slot. */
    t->pc = has_target ? target : o->pc + 4;
    prev = force_exit ? NULL : b; prev_edge = has_target ? edge : 0;
    goto block_start;
stop:
    CLOCK();
#undef WRITE
#undef CLOCK
#undef DISPATCH
#undef NEXT
}
