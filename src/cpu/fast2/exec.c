/* exec.c - fast2 L0 interpreter.  Quirks match quirk.c; device time matches hook_code by ticking
 * only at vv_hw_next_event() and running the instructions in between with vv_hw_set_now.
 * There is no I-cache or D-cache timing here (that lives in cpu/rtl, and only in a FIDELITY=rtl build). */
#include "fast2.h"
#include "decode.h"
#include "cpu/backend.h"
#include "cpu/service.h"
#include "cpu/irq.h"
#include "cpu/mips_decode.h"
#include "memmap.h"
#include "hw.h"
#include "quirk.h"
#include "dcache.h"
#include "profile.h"
#ifdef VV_FIDELITY_RTL
#include "cpu/rtl/rtl.h"
#endif
#include <string.h>

vv_cpu_kind_t vv_cpu_kind = VV_CPU_UNICORN;
void (*vv_code_invalidate)(uint32_t lo, uint32_t hi);

static uint32_t gpr[32], pc, hi, lo, c0_status, c0_epc;
static int stop_exc;               /* 1: CPU exception already printed; 2: unhandled memory fault */
static int q_lu, q_bal, q_unmapped, q_fetch, q_bios, q_ddr2, q_unaligned, q_lwlr;
static int q_ov, q_hilo, q_sys, q_res, q_cop0, q_jalr, q_lbsign, q_dc;

static int lu_on, lu_hold_sub;
static unsigned lu_r, lu_hold_r;
static uint32_t lu_old, lu_new, lu_pc, lu_hold_old;

static int br_pend, br_taken, br_in_ds, nullify;
static uint32_t br_tgt;

static int span_dead;
static uint32_t span_lo, span_hi;

uint32_t vv_fast2_gpr(unsigned r) { return r < 32 && r ? gpr[r] : 0; }
void vv_fast2_set_gpr(unsigned r, uint32_t v) { if (r && r < 32) gpr[r] = v; }
uint32_t vv_fast2_c0(void) { return c0_status; }
void vv_fast2_set_c0(uint32_t v) { c0_status = v; }
uint32_t vv_fast2_pc(void) { return pc; }
void vv_fast2_cancel_lu(void) { lu_on = lu_hold_sub = 0; br_pend = nullify = 0; }

uint32_t vv_view_gpr(uc_engine *uc, int r) {
    if (vv_cpu_kind == VV_CPU_UNICORN) { uint32_t v = 0; if (r > 0) uc_reg_read(uc, UC_MIPS_REG_0 + r, &v); return v; }
    return vv_fast2_gpr((unsigned)r);
}
uint32_t vv_view_pc(uc_engine *uc) {
    if (vv_cpu_kind == VV_CPU_UNICORN) { uint32_t v = 0; uc_reg_read(uc, UC_MIPS_REG_PC, &v); return v; }
    return pc;
}
uint32_t vv_view_hi(uc_engine *uc) {
    if (vv_cpu_kind == VV_CPU_UNICORN) { uint32_t v = 0; uc_reg_read(uc, UC_MIPS_REG_HI, &v); return v; }
    return hi;
}
uint32_t vv_view_lo(uc_engine *uc) {
    if (vv_cpu_kind == VV_CPU_UNICORN) { uint32_t v = 0; uc_reg_read(uc, UC_MIPS_REG_LO, &v); return v; }
    return lo;
}
uint32_t vv_view_status(uc_engine *uc) {
    if (vv_cpu_kind == VV_CPU_UNICORN) { uint32_t v = 0; uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &v); return v; }
    return c0_status;
}

static inline uint32_t RR(unsigned r) {
    if (lu_hold_sub && r == lu_hold_r) return lu_hold_old;
    return r ? gpr[r] : 0;
}
static inline void WR(unsigned r, uint32_t v) { if (r) gpr[r] = v; }

static int any(int cat) { return (vv_quirk_active() >> cat) & 1; }

static int exc(uint32_t intno) {
    vv_report_exception(intno, 0);
    pc = 0;
    stop_exc = 1;
    return 1;
}
/* tlb_fill refused the access: uc_emu_start returns UC_ERR_EXCEPTION and hook_intr does not run. */
static int mem_fault(void) {
    stop_exc = 2;
    return 1;
}
int vv_fast2_fetch_aborts(uint32_t at) {
    if (q_fetch || (at & 3)) return 0;
    int dead = 0;
    vv_fast2_fetch_raw(at, &dead);
    return dead;
}
int vv_fast2_unhandled(void) { return stop_exc == 2; }

static int prep(const Dec *d) {
    uint32_t und = vv_value_reads(d->insn) & ~vv_reg_defined;
    if (und) {
        unsigned r = (unsigned)__builtin_ctz(und);
        vv_reg_defined |= und;
        if (vv_quirk_hit(VV_Q_UNDEF_REG, d->pc, d->insn,
                "reads $%s, never written since reset (HW: 0 after FPGA configuration, the old value after a reset button press)%s",
                vv_gpr_name(r), und & ~(1u << r) ? " (and more)" : ""))
            return 1;
    }
    vv_reg_defined |= vv_writes(d->insn);
    lu_hold_sub = 0;
    if (lu_on) {
        unsigned r = lu_r;
        uint32_t old = lu_old, newv = lu_new, lpc = lu_pc;
        int used = (vv_reads(d->insn) >> r) & 1;
        lu_on = 0;
        if (used && any(VV_Q_LOAD_USE)) {
            if (vv_quirk_hit(VV_Q_LOAD_USE, d->pc, d->insn,
                    "reads $%s loaded by insn at 0x%08x (HW sees old 0x%08x, not 0x%08x)",
                    vv_gpr_name(r), lpc, old, newv))
                return 1;
        }
        if (used && q_lu) { lu_hold_sub = 1; lu_hold_r = r; lu_hold_old = old; }
    }
    return 0;
}

static void lu_make(unsigned rt, uint32_t old, uint32_t newv, uint32_t at) {
    if (!rt || !any(VV_Q_LOAD_USE)) return;
    lu_on = 1; lu_r = rt; lu_old = old; lu_new = newv; lu_pc = at;
}

/* ---- code pages: DDR 64K pages + 16 KiB BRAM pages.  A store bumps the page generation. ---- */
static uint32_t gen_ddr[65536];
static uint32_t gen_bram[16][4];

static uint32_t *gen_of(uint32_t addr) {
    uint32_t c = vv_canon_inl(addr);
    if (c == 0xFFFFFFFFu) return NULL;
    if ((c >> 28) == 1) {
        uint32_t pi = (c - 0x10000000u) >> 12;
        return pi < 65536u ? &gen_ddr[pi] : NULL;
    }
    return &gen_bram[c >> 28][(c >> 12) & 3];
}
static void bump_range(uint32_t lo, uint32_t hi) {
    if (hi < lo) return;
    for (uint32_t a = lo & ~0xFFFu; a < hi; ) {
        uint32_t *g = gen_of(a);
        if (g) (*g)++;
        uint32_t n = (a + 0x1000u) & ~0xFFFu;
        if (n <= a) break;
        a = n;
    }
}
static void fast2_inval(uint32_t lo, uint32_t hi) { bump_range(lo, hi); }

static int overlap(uint32_t a, unsigned n, uint32_t lo, uint32_t hi) {
    return n && a < hi && a + n > lo;
}

/* ---- basic blocks ---- */
#define BB_MAX 64
#define BB_SLOTS 4096
typedef struct {
    uint32_t start, gen, gen2, pc2;
    uint16_t n;
    uint8_t used;
    Dec d[BB_MAX];
} BB;
static BB bbs[BB_SLOTS];
/* Unicorn's TLB fill runs once per 4 KiB page, so unmapped-fetch is counted once per page. */
static uint32_t fetch_fill[1 << 15];

static int is_cti_op(int op) { return op == OP_B || op == OP_J || op == OP_JR; }
static int is_fence(int op) {
    return op == OP_MFC0 || op == OP_MTC0 || op == OP_ERET || op == OP_SYSCALL || op == OP_BREAK || op == OP_FETCHX;
}

static BB *build_bb(uint32_t start) {
    uint32_t slot = (start * 0x9E3779B1u) >> 20;          /* top bits, then linear probe */
    slot &= BB_SLOTS - 1;
    BB *b = NULL;
    for (int k = 0; k < 16; k++) {
        BB *s = &bbs[(slot + (unsigned)k) & (BB_SLOTS - 1)];
        if (!s->used || s->start == start) { b = s; break; }
    }
    if (!b) b = &bbs[slot];
    uint32_t *g = gen_of(start);
    b->used = 1; b->start = start; b->gen = g ? *g : 0; b->gen2 = 0; b->pc2 = 0; b->n = 0;
    uint32_t at = start;
    int want_delay = 0;
    for (int n = 0; n < BB_MAX; n++) {
        if (n && !want_delay && (at & ~0xFFFu) != (start & ~0xFFFu)) break;
        Dec dec;
        vv_decode_at(at, &dec);
        if (n && is_fence(dec.op)) break;
        b->d[b->n++] = dec;
        if ((at & ~0xFFFu) != (start & ~0xFFFu)) { b->pc2 = at; uint32_t *g2 = gen_of(at); b->gen2 = g2 ? *g2 : 0; }
        int cti = is_cti_op(dec.op);
        if (want_delay) break;
        if (cti) want_delay = 1;
        at += 4;
    }
    return b;
}
static BB *get_bb(uint32_t start) {
    uint32_t slot = (start * 0x9E3779B1u) >> 20;
    slot &= BB_SLOTS - 1;
    uint32_t *g = gen_of(start);
    uint32_t gv = g ? *g : 0;
    for (int k = 0; k < 16; k++) {
        BB *s = &bbs[(slot + (unsigned)k) & (BB_SLOTS - 1)];
        if (!s->used) break;
        if (s->start != start || s->gen != gv) continue;
        if (s->pc2) { uint32_t *g2 = gen_of(s->pc2); if ((g2 ? *g2 : 0) != s->gen2) continue; }
        return s;
    }
    return build_bb(start);
}

/* ---- ALU ---- */
static uint32_t alu(int id, uint32_t a, uint32_t b, unsigned sa) {
    switch (id) {
    case A_ADD: case A_ADDU: return a + b;
    case A_SUB: case A_SUBU: return a - b;
    case A_AND: return a & b;
    case A_OR: return a | b;
    case A_XOR: return a ^ b;
    case A_NOR: return ~(a | b);
    case A_SLT: return (int32_t)a < (int32_t)b;
    case A_SLTU: return a < b;
    case A_SLL: return b << sa;
    case A_SRL: return b >> sa;
    case A_SRA: return (uint32_t)((int32_t)b >> sa);
    case A_SLLV: return b << (a & 31);
    case A_SRLV: return b >> (a & 31);
    case A_SRAV: return (uint32_t)((int32_t)b >> (a & 31));
    case A_ROTR: return sa ? (b >> sa) | (b << (32 - sa)) : b;
    case A_ROTRV: { unsigned s = a & 31; return s ? (b >> s) | (b << (32 - s)) : b; }
    default: return 0;
    }
}

static int do_alu(const Dec *d) {
    uint32_t a = RR(d->rs), b = RR(d->rt), w;
    unsigned sa = d->sa;
    if (d->alu == A_ROTR || d->alu == A_ROTRV) {
        uint32_t srl = d->alu == A_ROTR ? b >> sa : b >> (a & 31);
        if (any(VV_Q_RESERVED)) {
            if (vv_quirk_hit(VV_Q_RESERVED, d->pc, d->insn, "HW does %s: 0x%08x", d->alu == A_ROTR ? "srl" : "srlv", srl))
                return 1;
        }
        if (q_res) { WR(d->rd, srl); return 0; }
    }
    if (d->alu == A_ADD || d->alu == A_SUB) {
        int32_t tmp;
        int ov = d->alu == A_ADD ? __builtin_add_overflow((int32_t)a, (int32_t)b, &tmp)
                                 : __builtin_sub_overflow((int32_t)a, (int32_t)b, &tmp);
        w = d->alu == A_ADD ? a + b : a - b;
        if (ov) {
            if (any(VV_Q_OVERFLOW) && vv_quirk_hit(VV_Q_OVERFLOW, d->pc, d->insn,
                    "0x%08x %c 0x%08x overflows; HW wraps to 0x%08x", a, d->alu == A_ADD ? '+' : '-', b, w))
                return 1;
            if (!q_ov) return exc(22);
        }
        WR(d->rd, w);
        return 0;
    }
    if (d->alu == A_SLL || d->alu == A_SRL || d->alu == A_SRA || d->alu == A_ROTR) w = alu(d->alu, a, b, sa);
    else w = alu(d->alu, a, b, sa);
    WR(d->rd, w);
    return 0;
}

static int do_imm(const Dec *d) {
    uint32_t a = RR(d->rs), w = alu(d->alu, a, (uint32_t)d->imm, 0);
    if (d->alu == A_ADD) {
        int32_t tmp;
        int ov = __builtin_add_overflow((int32_t)a, d->imm, &tmp);
        if (ov) {
            if (any(VV_Q_OVERFLOW) && vv_quirk_hit(VV_Q_OVERFLOW, d->pc, d->insn,
                    "0x%08x + %d overflows; HW wraps to 0x%08x", a, d->imm, w))
                return 1;
            if (!q_ov) return exc(22);
        }
    }
    WR(d->rt, w);
    return 0;
}

/* ---- branches ---- */
static int taken_cond(const Dec *d) {
    uint32_t a = RR(d->rs), b = RR(d->rt);
    int32_t s = (int32_t)a;
    switch (d->alu) {
    case B_EQ: return a == b;
    case B_NE: return a != b;
    case B_LEZ: return s <= 0;
    case B_GTZ: return s > 0;
    case B_LTZ: return s < 0;
    case B_GEZ: return s >= 0;
    case B_REGIMM: return (d->rt & 1) ? s >= 0 : s < 0;
    default: return 0;
    }
}

static int do_branch(const Dec *d, int in_ds) {
    (void)in_ds;
    int taken = taken_cond(d);
    uint32_t tgt = d->pc + 4 + ((uint32_t)d->imm << 2);
    int link = (d->flags & FL_LINK) != 0;
    /* REGIMM rt other than 0/1/16/17 is the reserved "traps branch" behaviour.
     * beq/bne/blez/bgtz use rt as a register, not a REGIMM code. */
    int weird = d->alu == B_REGIMM && d->rt > 1 && d->rt != 16 && d->rt != 17;
    if (weird && any(VV_Q_RESERVED)) {
        if (vv_quirk_hit(VV_Q_RESERVED, d->pc, d->insn, "HW: %s%s, %s",
                (d->rt & 1) ? "bgez" : "bltz", link ? "al" : "", taken ? "taken" : "not taken"))
            return 1;
    }
    if (link && !taken && (d->rt == 16 || d->rt == 17) && any(VV_Q_BAL_UNTAKEN)) {
        if (vv_quirk_hit(VV_Q_BAL_UNTAKEN, d->pc, d->insn,
                "not taken; HW leaves $ra = 0x%08x (MIPS32 links 0x%08x)", gpr[31], d->pc + 8))
            return 1;
    }
    int do_link = link && (taken || !q_bal);
    /* Reserved REGIMM, not taken: the following insn runs but is not a delay slot (quirk.c skip_insn).
     * Real bltzal/bgezal (rt 16/17) stay real delay slots. */
    int as_delay = 1;
    if (weird && q_res && !taken) as_delay = 0;
    if ((d->flags & FL_LIKELY) && !taken && !q_res) { nullify = 1; do_link = 0; taken = 0; as_delay = 1; }
    if (do_link) WR(31, d->pc + 8);
    br_taken = taken; br_tgt = tgt; br_pend = 1; br_in_ds = as_delay;
    if (!as_delay) { br_pend = 1; br_in_ds = 0; }   /* still sequence into the next insn, then fall through */
    return 0;
}

static int do_j(const Dec *d) {
    if (d->flags & FL_LINK) WR(31, d->pc + 8);
    br_taken = 1; br_tgt = (uint32_t)d->imm; br_pend = 1; br_in_ds = 1;
    return 0;
}
static int do_jr(const Dec *d, int in_ds) {
    (void)in_ds;
    uint32_t tgt = RR(d->rs);
    if (tgt & 3) return exc(12);                    /* L0: misaligned jr/jalr is AdEL */
    int link_rd = d->rd;
    if ((d->flags & FL_LINK) && d->rd != 31 && any(VV_Q_JALR_RD)) {
        if (vv_quirk_hit(VV_Q_JALR_RD, d->pc, d->insn, "links to $%s; old HW links to $ra", vv_gpr_name(d->rd)))
            return 1;
        if (q_jalr) link_rd = 31;
    }
    if (d->flags & FL_LINK) WR((unsigned)link_rd, d->pc + 8);
    br_taken = 1; br_tgt = tgt; br_pend = 1; br_in_ds = 1;
    return 0;
}

/* ---- memory ---- */
static void note_store_code(uint32_t addr, unsigned size) {
    bump_range(addr, addr + size);
    if (span_hi > span_lo && overlap(addr, size, span_lo, span_hi)) span_dead = 1;
}

static uint32_t load_ram(uint32_t addr, unsigned size, int sign, int *ok) {
    uint32_t c = vv_canon_inl(addr);
    uint8_t *p = vv_ram(c, size);
    if (!p) { *ok = 0; return 0; }
    *ok = 1;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = (v << 8) | p[i];
    if (sign && size == 1 && (v & 0x80)) v |= 0xFFFFFF00u;
    if (sign && size == 2 && (v & 0x8000)) v |= 0xFFFF0000u;
    return v;
}
static void store_ram(uint32_t addr, unsigned size, uint32_t v) {
    uint32_t c = vv_canon_inl(addr);
    uint8_t *p = vv_ram(c, size);
    if (!p) return;
    if (size == 4) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
    else if (size == 2) { p[0] = v >> 8; p[1] = v; }
    else p[0] = (uint8_t)v;
    note_store_code(c, size);
}

static uint32_t be_word(uint32_t addr, int *ok) {
    uint32_t c = vv_canon_inl(addr & ~3u);
    return vv_ram_be32(c, ok);
}

static int do_mem(uc_engine *uc, const Dec *d) {
    int store = (d->flags & FL_STORE) != 0;
    uint32_t addr = (d->flags & FL_ADDR0) ? 0 : RR(d->rs) + (uint32_t)d->imm;
    unsigned size = d->alu;
    uint32_t old_rt = gpr[d->rt];
    if (d->flags & FL_ADDR0) {
        if (any(VV_Q_RESERVED) && vv_quirk_hit(VV_Q_RESERVED, d->pc, d->insn, "HW %s address 0 (%s)",
                store ? "stores to" : "loads from", store ? "dropped" : "reads 0"))
            return 1;
        if (q_res) {
            if (!store) { WR(d->rt, 0); lu_make(d->rt, old_rt, 0, d->pc); }
            return 0;
        }
    }
    if ((d->flags & (FL_LWL | FL_LWR | FL_SWL | FL_SWR)) && q_lwlr) {
        if (any(VV_Q_LWLR) && vv_quirk_hit(VV_Q_LWLR, d->pc, d->insn, "addr=0x%08x; HW %s", addr,
                store ? "stores nothing" : "writes 0 to rt"))
            return 1;
        if (q_lwlr) {
            uint32_t c = vv_canon_inl(addr);
            if (c != 0xFFFFFFFFu && (c >> 28) == 8) vv_mmio_read(uc, c & 0x3FFFu, 4);
            if (!store) { WR(d->rt, 0); lu_make(d->rt, old_rt, 0, d->pc); }
            return 0;
        }
    }
    if (size > 1 && (addr & (size - 1)) && !(d->flags & (FL_LWL | FL_LWR | FL_SWL | FL_SWR))) {
        if (any(VV_Q_UNALIGNED) && vv_quirk_hit(VV_Q_UNALIGNED, d->pc, d->insn, "addr=0x%08x size %u; HW %s",
                addr, size, store ? "drops the store" : "loads 0"))
            return 1;
        if (q_unaligned) {
            uint32_t c = vv_canon_inl(addr);
            if (c != 0xFFFFFFFFu && (c >> 28) == 8) vv_mmio_read(uc, c & 0x3FFFu, size ? size : 4);
            if (!store) { WR(d->rt, 0); lu_make(d->rt, old_rt, 0, d->pc); }
            return 0;
        }
        return exc(store ? 13 : 12);
    }
    uint32_t c = vv_canon_inl(addr);
    int mmio = c != 0xFFFFFFFFu && (c >> 28) == 8;
    if (c == 0xFFFFFFFFu) {
        if (any(VV_Q_UNMAPPED) && vv_quirk_hit(VV_Q_UNMAPPED, d->pc, d->insn, "%s of undecoded 0x%08x; HW %s",
                store ? "store" : "load", addr, store ? "drops it" : "reads 0"))
            return 1;
        if (q_unmapped) {
            if (!store) { WR(d->rt, 0); lu_make(d->rt, old_rt, 0, d->pc); }
            return 0;
        }
        return mem_fault();
    }
    if ((addr >> 28) == 2 && (store ? (d->pc >> 28) != 4 : 1)) {
        if (any(VV_Q_DDR2) && vv_quirk_hit(VV_Q_DDR2, d->pc, d->insn, "%s 0x%08x; HW %s",
                store ? "store to" : "load from", addr,
                store ? "drops it (not running from BIOS)" : "returns stale D-cache data"))
            return 1;
        if (q_ddr2 && store) return 0;          /* dropped: no write, no invalidate */
        if (q_ddr2 && !store) vv_quirk_not_emulated(VV_Q_DDR2, d->pc, d->insn, "load via the I-cache path returns DDR data");
    }
    if (store && (c >> 28) == 4) {
        uint32_t val = RR(d->rt);
        unsigned nb = size ? size : 4;
        uint32_t shown = nb >= 4 ? val : val & ((1u << (8 * nb)) - 1);
        if (any(VV_Q_BIOS_STORE) && vv_quirk_hit(VV_Q_BIOS_STORE, d->pc, d->insn,
                "store to 0x%08x (val 0x%08x); HW drops it", addr, val))
            return 1;
        vv_warn_bios_write(addr, (int)nb, shown);
        if (q_bios) return 0;
    }
    if (store && mmio && size && size < 4 && (addr & 3) + size != 4 && any(VV_Q_MMIO_LANE)) {
        uint32_t v = RR(d->rt) & ((1u << (8 * size)) - 1);
        uint32_t placed = v << (8 * (4 - size - (addr & 3)));
        if (vv_quirk_hit(VV_Q_MMIO_LANE, d->pc, d->insn, "%s to MMIO 0x%08x (val 0x%x); HW delivers 0x%08x%s",
                size == 1 ? "sb" : "sh", addr, v, placed,
                (c & VV_MMIO_DECODE) == VV_OW_UATX_DATA ? ", UART sends 0x00" :
                ((c & VV_MMIO_DECODE) & ~0x7Fu) == VV_OW_LCD_BASE ? ", LCD char gets 0x00" : ""))
            return 1;
    }
    /* D-cache functional model (dcache-stale).  Not the RTL timing tags. */
    int aligned = size && !(addr & (size - 1));
    uint32_t stale_v = 0; int have_stale = 0;
    if (aligned && (c >> 28) == 1 && ((addr >> 28) == 1 || (addr >> 28) == 3)) {
        if (store) vv_dc_store(c, size, RR(d->rt));
        else {
            uint32_t old, cur; int ok = 1;
            if (vv_dc_load(c & ~3u, &old) && (cur = vv_ram_be32(c & ~3u, &ok), ok) && old != cur) {
                unsigned sh = 8 * (4 - size - (addr & 3));
                uint32_t v = size == 4 ? old : (old >> sh) & ((1u << (8 * size)) - 1);
                if ((d->flags & FL_SIGN) && size == 1 && (v & 0x80u) && !q_lbsign) v |= 0xFFFFFF00u;
                if ((d->flags & FL_SIGN) && size == 2 && (v & 0x8000u) && !q_lbsign) v |= 0xFFFF0000u;
                if (vv_quirk_hit(VV_Q_DCACHE_STALE, d->pc, d->insn,
                        "load of 0x%08x hits a D-cache line the GPU wrote behind the cache: HW returns the cached 0x%08x (DDR has 0x%08x)",
                        addr, old, cur))
                    return 1;
                if (q_dc) { stale_v = v; have_stale = 1; }
            }
        }
    }
    if (d->flags & (FL_LWL | FL_LWR | FL_SWL | FL_SWR)) {
        int ok = 1;
        uint32_t mem = be_word(addr, &ok);
        unsigned o = addr & 3;
        if (d->flags & FL_LWL) {
            static const uint32_t msk[] = {0, 0xFF, 0xFFFF, 0xFFFFFF};
            uint32_t v = o == 0 ? mem : (mem << (8 * o)) | (RR(d->rt) & msk[o]);
            WR(d->rt, v); lu_make(d->rt, old_rt, v, d->pc);
        } else if (d->flags & FL_LWR) {
            static const uint32_t msk[] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0};
            uint32_t v = o == 3 ? mem : (RR(d->rt) & msk[o]) | (mem >> (8 * (3 - o)));
            WR(d->rt, v); lu_make(d->rt, old_rt, v, d->pc);
        } else if (d->flags & FL_SWL) {
            /* big-endian swl: replaces the high bytes of the word ending at this address */
            uint32_t w = ok ? mem : 0, val = RR(d->rt);
            uint32_t nw = o == 0 ? val : (w & ((1u << (8 * o)) - 1)) | (val >> (8 * o));
            /* simpler byte merge */
            uint8_t bytes[4];
            bytes[0] = w >> 24; bytes[1] = w >> 16; bytes[2] = w >> 8; bytes[3] = w;
            uint8_t src[4] = { val >> 24, val >> 16, val >> 8, val };
            for (unsigned i = 0; i <= o; i++) bytes[i] = src[3 - o + i];
            nw = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
            store_ram(addr & ~3u, 4, nw);
        } else {
            uint32_t w = ok ? mem : 0, val = RR(d->rt);
            uint8_t bytes[4] = { w >> 24, w >> 16, w >> 8, w };
            uint8_t src[4] = { val >> 24, val >> 16, val >> 8, val };
            for (unsigned i = o; i < 4; i++) bytes[i] = src[i - o];
            uint32_t nw = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
            store_ram(addr & ~3u, 4, nw);
        }
        return 0;
    }
    if (mmio) {
        uint64_t off = c & 0x3FFFu;
        if (store) vv_mmio_write(uc, off, size, RR(d->rt));
        else {
            uint32_t v = (uint32_t)vv_mmio_read(uc, off, size);
            WR(d->rt, v); lu_make(d->rt, old_rt, gpr[d->rt], d->pc);
        }
        return 0;
    }
    if (store) {
        if ((c >> 28) == 4 && q_bios) return 0;
        store_ram(addr, size, RR(d->rt));
        return 0;
    }
    int ok = 1;
    uint32_t v;
    if ((d->flags & FL_SIGN) && (q_lbsign || any(VV_Q_LB_SIGN))) {
        uint32_t wide = have_stale ? stale_v : load_ram(addr, size, 1, &ok);
        uint32_t zx = wide & (size == 1 ? 0xFFu : 0xFFFFu);
        if (wide != zx && any(VV_Q_LB_SIGN)) {
            if (vv_quirk_hit(VV_Q_LB_SIGN, d->pc, d->insn, "loaded 0x%08x; old HW gives 0x%08x", wide, zx))
                return 1;
        }
        v = q_lbsign ? zx : wide;
    } else {
        int sign = (d->flags & FL_SIGN) && !q_lbsign;
        v = have_stale ? stale_v : load_ram(addr, size, sign, &ok);
    }
    WR(d->rt, v);
    lu_make(d->rt, old_rt, v, d->pc);
    return 0;
}

static int do_hilo(const Dec *d) {
    if (d->alu == H_MFHI || d->alu == H_MFLO) {
        if (any(VV_Q_HILO) && vv_quirk_hit(VV_Q_HILO, d->pc, d->insn, "HW writes 0 to $%s", vv_gpr_name(d->rd)))
            return 1;
        if (q_hilo) { WR(d->rd, 0); return 0; }
        WR(d->rd, d->alu == H_MFHI ? hi : lo);
        return 0;
    }
    if (any(VV_Q_HILO) && vv_quirk_hit(VV_Q_HILO, d->pc, d->insn, "HW: no-op")) return 1;
    if (q_hilo) return 0;
    uint32_t a = RR(d->rs), b = RR(d->rt);
    switch (d->alu) {
    case H_MTHI: hi = a; break;
    case H_MTLO: lo = a; break;
    case H_MULT: { int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b; lo = (uint32_t)p; hi = (uint32_t)((uint64_t)p >> 32); break; }
    case H_MULTU: { uint64_t p = (uint64_t)a * b; lo = (uint32_t)p; hi = (uint32_t)(p >> 32); break; }
    case H_DIV:
        if (!b) { lo = 0; hi = a; }
        else if (a == 0x80000000u && b == 0xFFFFFFFFu) { lo = 0x80000000u; hi = 0; }
        else { lo = (uint32_t)((int32_t)a / (int32_t)b); hi = (uint32_t)((int32_t)a % (int32_t)b); }
        break;
    case H_DIVU:
        if (!b) { lo = 0xFFFFFFFFu; hi = a; }
        else { lo = a / b; hi = a % b; }
        break;
    default: break;
    }
    return 0;
}

static int do_cop0(const Dec *d, int is_mtc) {
    unsigned rd = d->rd, rt = d->rt;
    int odd = (d->insn & 7) || !(rd == 9 || rd == 11 || rd == 12 || rd == 13 || rd == 14) || (is_mtc && rd == 14);
    if (odd && any(VV_Q_COP0) && vv_quirk_hit(VV_Q_COP0, d->pc, d->insn, "%s $%u%s: HW %s",
            is_mtc ? "mtc0" : "mfc0", rd, (d->insn & 7) ? " sel!=0" : "",
            is_mtc ? "ignores it" : "reads COP0150 register (0 if none)"))
        return 1;
    if (is_mtc) {
        uint32_t v = RR(rt);
        if (q_cop0) vv_cop0_write(rd, v);
        else if ((d->pc >> 28) == 0xC && (rd == 13 || rd == 14)) {
            if (rd == 13) vv_cop0_write(13, v); else vv_hw_set_epc(v);
        }
        if (rd == 12) c0_status = v;            /* Unicorn's CP0 Status tracks mtc0 even when the shadow is separate */
        if (!q_cop0 && rd == 14) c0_epc = v;    /* quirk off: EPC is a real CP0 register */
        return 0;
    }
    uint32_t v = 0;
    if (q_cop0) v = vv_cop0_read(rd);
    else if (rd == 12) v = c0_status;
    else if ((d->pc >> 28) == 0xC && rd == 13) v = vv_hw_cause();
    else if ((d->pc >> 28) == 0xC && rd == 14) v = vv_hw_epc();
    else if (rd == 14) v = c0_epc;
    WR(rt, v);
    return 0;
}

static int do_eret(const Dec *d, int in_ds) {
    if (d->insn == 0x42000018u && vv_hw_in_isr()) {
        uint32_t st = q_cop0 ? vv_hw_status() : c0_status;
        vv_hw_isr_exit_check(st, d->pc, "eret");
        if (vv_quirk_halted) return 1;
    }
    if (any(VV_Q_RESERVED) && vv_quirk_hit(VV_Q_RESERVED, d->pc, d->insn, "COP0 op: HW nop")) return 1;
    if (q_res && !in_ds) return 0;
    if (q_res && in_ds) vv_quirk_not_emulated(VV_Q_RESERVED, d->pc, d->insn, "in a branch delay slot");
    pc = q_cop0 ? vv_hw_epc() : c0_epc;
    c0_status &= ~2u;
    br_pend = 0; br_taken = 0;
    return 2;                                   /* pc already set; caller must not do pc+4 */
}

static int do_sys(const Dec *d, int in_ds) {
    if (any(VV_Q_SYSCALL) && vv_quirk_hit(VV_Q_SYSCALL, d->pc, d->insn, "HW: nop")) return 1;
    if (q_sys && !in_ds) return 0;
    if (q_sys && in_ds) vv_quirk_not_emulated(VV_Q_SYSCALL, d->pc, d->insn, "in a branch delay slot");
    return exc(17);
}

static int do_rd0(const Dec *d, int in_ds) {
    if (any(VV_Q_RESERVED) && vv_quirk_hit(VV_Q_RESERVED, d->pc, d->insn, "SPECIAL funct 0x%02x: HW writes 0 to $%s",
            d->insn & 63, vv_gpr_name(d->rd)))
        return 1;
    if (q_res && !in_ds) { WR(d->rd, 0); return 0; }
    if (q_res && in_ds) vv_quirk_not_emulated(VV_Q_RESERVED, d->pc, d->insn, "in a branch delay slot");
    if ((d->insn & 63) == 10 || (d->insn & 63) == 11) {
        int movn = (d->insn & 63) == 11;
        if (movn ? RR(d->rt) != 0 : RR(d->rt) == 0) WR(d->rd, RR(d->rs));
        return 0;
    }
    return exc(20);
}

static int do_trap(const Dec *d) {
    int32_t a = (int32_t)RR(d->rs), imm = d->imm;
    int take = 0;
    switch (d->rt) {
    case 8: take = a >= imm; break;
    case 9: take = (uint32_t)a >= (uint32_t)imm; break;
    case 10: take = a < imm; break;
    case 11: take = (uint32_t)a < (uint32_t)imm; break;
    case 12: take = a == imm; break;
    case 14: take = a != imm; break;
    default: take = 0; break;
    }
    if (take) return exc(23);
    return 0;
}

/* Execute d.  Returns 0 continue (pc set to the next fetch), 1 stop. */
static int exec_dec(uc_engine *uc, const Dec *d) {
    VV_PROF_COUNT(VV_PS_FAST2);
    int was_delay = br_pend;
    int in_ds = was_delay && br_in_ds;
    int pend_taken = br_taken;
    uint32_t pend_tgt = br_tgt;
    br_pend = 0;
    if (nullify) {
        nullify = 0;
        pc = d->pc + 4;
        if (was_delay && pend_taken) pc = pend_tgt;
        vv_quirk_set_last_pc(d->pc);
        return 0;
    }
    if (d->op != OP_FETCH0 && d->op != OP_FETCHX && prep(d)) return 1;
    int rc = 0, pc_set = 0;
    switch (d->op) {
    case OP_NOP: break;
    case OP_ALU: rc = do_alu(d); break;
    case OP_IMM: rc = do_imm(d); break;
    case OP_LUI: WR(d->rt, (uint32_t)d->imm); break;
    case OP_B: rc = do_branch(d, in_ds); break;
    case OP_J: rc = do_j(d); break;
    case OP_JR: rc = do_jr(d, in_ds); break;
    case OP_MEM: rc = do_mem(uc, d); break;
    case OP_MFC0: rc = do_cop0(d, 0); break;
    case OP_MTC0: rc = do_cop0(d, 1); break;
    case OP_ERET: rc = do_eret(d, in_ds); pc_set = rc == 2; if (rc == 2) rc = 0; break;
    case OP_SYSCALL: rc = do_sys(d, in_ds); break;
    case OP_BREAK: rc = exc(18); break;
    case OP_HILO: rc = do_hilo(d); break;
    case OP_MOVZ:
        if ((d->alu ? RR(d->rt) != 0 : RR(d->rt) == 0)) WR(d->rd, RR(d->rs));
        break;
    case OP_RD0: rc = do_rd0(d, in_ds); break;
    case OP_TRAP: rc = do_trap(d); break;
    case OP_RI:
        if (any(VV_Q_RESERVED) && vv_quirk_hit(VV_Q_RESERVED, d->pc, d->insn, "HW nop")) return 1;
        if (q_res && in_ds) vv_quirk_not_emulated(VV_Q_RESERVED, d->pc, d->insn, "in a branch delay slot");
        if (q_res && !in_ds) break;
        rc = exc(20); break;
    case OP_RNOP:
        if (any(VV_Q_RESERVED) && vv_quirk_hit(VV_Q_RESERVED, d->pc, d->insn, "HW nop")) return 1;
        if (q_res && in_ds) vv_quirk_not_emulated(VV_Q_RESERVED, d->pc, d->insn, "in a branch delay slot");
        if (q_res) break;                   /* nop, including a delay slot this model does not execute */
        rc = exc(20); break;
    case OP_COP0X:
        if (any(VV_Q_RESERVED) && vv_quirk_hit(VV_Q_RESERVED, d->pc, d->insn, "COP0 op: HW nop")) return 1;
        if (q_res && !in_ds) break;
        if (q_res && in_ds) vv_quirk_not_emulated(VV_Q_RESERVED, d->pc, d->insn, "in a branch delay slot");
        rc = exc(20); break;
    case OP_FETCH0: {
        uint32_t page = d->pc >> 12;
        uint32_t bit = 1u << (page & 31);
        if (!(fetch_fill[page >> 5] & bit)) {
            fetch_fill[page >> 5] |= bit;
            vv_quirk_fetch(uc, d->pc, (vv_canon_inl(d->pc) == 0xFFFFFFFFu) ? "undecoded" : "MMIO");
            if (vv_quirk_halted) return 1;
        }
        break;
    }
    case OP_FETCHX: rc = exc(12); break;
    default: rc = exc(20); break;
    }
    if (rc) return 1;
    vv_quirk_set_last_pc(d->pc);
    if (!pc_set) {
        if (!br_pend) {
            pc = d->pc + 4;
            if (was_delay && pend_taken) pc = pend_tgt;
        } else {
            pc = d->pc + 4;                     /* delay slot is next */
        }
    }
    if (vv_guest_stopping() || vv_quirk_halted || stop_exc) return 1;
    return 0;
}

int vv_fast2_step(uc_engine *uc, uint32_t *pc_io) {
    pc = *pc_io;
    Dec d; vv_decode_at(pc, &d);
    int stop = exec_dec(uc, &d);
    *pc_io = pc;
    return stop;
}

/* MMIO and COP0/syscall fences read the cycle clock, so they stay on the outer tick.
 * A cacheable load or store does not. */
static int needs_tick(const Dec *d) {
    if (is_fence(d->op)) return 1;
    if (d->op != OP_MEM) return 0;
    uint32_t base = d->rs ? gpr[d->rs] : 0;
    uint32_t addr = base + (uint32_t)d->imm;
    if ((addr >> 28) == 8) return 1;
    uint32_t c = vv_canon_inl(addr);
    return c != 0xFFFFFFFFu && (c >> 28) == 8;
}

/* The outer loop already counted and ticked the instruction at pc.  Execute it, then
 * keep fetching blocks until `budget` more instructions have run or the next one needs
 * a real vv_hw_tick.  One basic block is not enough: a two-instruction loop would
 * otherwise pay the full service path on every back edge. */
static int span(uc_engine *uc, uint64_t budget) {
    int pending = br_pend || nullify;
    uint64_t used = 0;
    int boot = 1;
    for (;;) {
        BB *b = get_bb(pc);
        if (!b || !b->n) return 1;
        unsigned i = 0;
        if (boot) {
            span_dead = 0;
            span_lo = b->n > 1 ? b->d[1].pc : 0;
            span_hi = b->d[b->n - 1].pc + 4;
            if (exec_dec(uc, &b->d[0])) return 1;
            boot = 0;
            if (pending) { vv_hw_set_now(vv_insns()); return 0; }
            if (b->n == 1 || pc != b->d[1].pc) {
                if (used >= budget) { vv_hw_set_now(vv_insns()); return 0; }
                continue;
            }
            i = 1;
        } else {
            span_dead = 0;
            span_lo = b->d[0].pc;
            span_hi = b->d[b->n - 1].pc + 4;
        }
        int hopped = 0;
        for (; i < b->n && used < budget; i++) {
            if (vv_quirk_halted || stop_exc) return 1;
            if (span_dead || pc != b->d[i].pc || needs_tick(&b->d[i])) {
                vv_hw_set_now(vv_insns());
                return 0;
            }
            if (vv_fast2_fetch_aborts(b->d[i].pc)) { stop_exc = 2; return 1; }
            if (vv_svc_quiet(b->d[i].pc)) return 1;
            used++;
            /* irq_step and the event trace read the cycle clock.  Quiet instructions have
             * not been ticked, so publish the instruction count before sampling IE/IP. */
            vv_hw_set_now(vv_insns());
            if (vv_irqs_on()) {
                uint32_t npc;
                if (vv_irq_step(NULL, b->d[i].pc, &npc)) {
                    vv_fast2_cancel_lu();
                    vv_quirk_cancel();
                    pc = npc;
                    vv_hw_set_now(vv_insns());
                    return 0;
                }
            }
            int was_br = br_pend;
            span_lo = b->d[i].pc;
            span_hi = b->d[b->n - 1].pc + 4;
            if (exec_dec(uc, &b->d[i])) return 1;
            if (was_br) { hopped = 1; break; }
        }
        if (vv_quirk_halted || stop_exc) return 1;
        if (!hopped && i < b->n) { vv_hw_set_now(vv_insns()); return 0; }
        if (used >= budget) { vv_hw_set_now(vv_insns()); return 0; }
    }
}

void vv_fast2_init(uc_engine *uc, uint32_t entry) {
    memset(gpr, 0, sizeof gpr);
    for (int i = 1; i < 32; i++) { uint32_t v = 0; uc_reg_read(uc, UC_MIPS_REG_0 + i, &v); gpr[i] = v; }
    uc_reg_read(uc, UC_MIPS_REG_HI, &hi);
    uc_reg_read(uc, UC_MIPS_REG_LO, &lo);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &c0_status);
    pc = entry;
    c0_epc = 0;
    stop_exc = 0;
    lu_on = lu_hold_sub = br_pend = br_taken = nullify = 0;
    q_lu = vv_quirk_on(VV_Q_LOAD_USE);
    q_bal = vv_quirk_on(VV_Q_BAL_UNTAKEN);
    q_unmapped = vv_quirk_on(VV_Q_UNMAPPED);
    q_fetch = vv_quirk_on(VV_Q_UNMAPPED_FETCH);
    q_bios = vv_quirk_on(VV_Q_BIOS_STORE);
    q_ddr2 = vv_quirk_on(VV_Q_DDR2);
    q_unaligned = vv_quirk_on(VV_Q_UNALIGNED);
    q_lwlr = vv_quirk_on(VV_Q_LWLR);
    q_ov = vv_quirk_on(VV_Q_OVERFLOW);
    q_hilo = vv_quirk_on(VV_Q_HILO);
    q_sys = vv_quirk_on(VV_Q_SYSCALL);
    q_res = vv_quirk_on(VV_Q_RESERVED);
    q_cop0 = vv_quirk_on(VV_Q_COP0);
    q_jalr = vv_quirk_on(VV_Q_JALR_RD);
    q_lbsign = vv_quirk_on(VV_Q_LB_SIGN);
    q_dc = vv_quirk_on(VV_Q_DCACHE_STALE);
    memset(bbs, 0, sizeof bbs);
    memset(fetch_fill, 0, sizeof fetch_fill);
    vv_code_invalidate = fast2_inval;

}

uc_err vv_fast2_run(uc_engine *uc, uint32_t entry, uint32_t until, int has_until) {
    vv_fast2_init(uc, entry);
    while (!vv_quirk_halted && !stop_exc) {
        if (has_until && pc == until) break;
        uint64_t tick_at = vv_insns() + 1;
        if (vv_cpu_kind == VV_CPU_RTL) break;   /* rtl runner calls vv_fast2_step itself */
        if (vv_fast2_fetch_aborts(pc)) { stop_exc = 2; break; }
        if (vv_svc_enter_at(uc, pc, tick_at)) break;
        uint32_t npc = pc;
        if (vv_irqs_on() && vv_irq_step(NULL, pc, &npc)) {
            vv_fast2_cancel_lu();
            vv_quirk_cancel();
            if (!vv_quirk_halted && vv_svc_leave(uc, pc)) break;
            if (vv_quirk_halted) break;
            pc = npc;
            continue;
        }
        if (vv_quirk_halted) break;
        if (vv_svc_leave(uc, pc)) break;
        uint64_t budget = vv_svc_quiet_budget();
        if (span(uc, budget)) break;
    }
    vv_hw_set_now(vv_insns());
    /* hook_intr reports a CPU exception and uc_emu_stop returns OK. A refused TLB fill does not. */
    if (stop_exc == 2) return UC_ERR_EXCEPTION;
    return UC_ERR_OK;
}

uc_err vv_cpu_run(uc_engine *uc, uint32_t entry, uint32_t until, int has_until) {
#ifdef VV_FIDELITY_RTL
    if (vv_cpu_kind == VV_CPU_RTL) return vv_rtl_run(uc, entry, until, has_until);
#endif
    return vv_fast2_run(uc, entry, until, has_until);
}
