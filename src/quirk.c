/* quirk.c - emulate and/or report MIPS150 hardware quirks (see quirk.h for the list and the
 * hardware references).
 *
 * Mechanism: the existing UC_HOOK_CODE calls vv_quirk_post() (fix up the result of the PREVIOUS
 * instruction), then irq_step(), then vv_quirk_pre() (decode the instruction about to run).
 * pre() reports a quirk and/or arranges its hardware result:
 *   - register fix-ups after the insn ran (restore a register / set a result), HI/LO restore,
 *     memory restore (dropped stores into RAM), VOID page re-zero (dropped stores elsewhere);
 *   - base-register redirect to the VOID page for accesses Unicorn would fault on (unaligned,
 *     lwl/lwr/swl/swr), restored afterwards;
 *   - skipping the insn (PC := pc+4) when the hardware treats it as a nop (not in a delay slot);
 *   - the load-use swap: the insn after a load gets the OLD rt while it runs, then the loaded value.
 * Cost with nothing pending: one 4-byte read + decode per instruction. */
#include "term.h"
#include "quirk.h"
#include "memmap.h"
#include "hw.h"
#include "dcache.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

uint32_t vv_quirk_emul, vv_quirk_detect, vv_quirk_halt;
int vv_quirk_halted;

static const char *qname[VV_Q_COUNT] = {
#define Q(id, n, def, bug, d) n,
    VV_QUIRK_TABLE
#undef Q
};
static const char *qdesc[VV_Q_COUNT] = {
#define Q(id, n, def, bug, d) d,
    VV_QUIRK_TABLE
#undef Q
};
static const uint8_t qdef[VV_Q_COUNT] = {
#define Q(id, n, def, bug, d) def,
    VV_QUIRK_TABLE
#undef Q
};
static const uint8_t qbug[VV_Q_COUNT] = {
#define Q(id, n, def, bug, d) bug,
    VV_QUIRK_TABLE
#undef Q
};
static uint64_t qhits[VV_Q_COUNT], qnoemu[VV_Q_COUNT];
#define ALL ((1u << VV_Q_COUNT) - 1)
#define E(id) vv_quirk_on(VV_Q_##id)
#define D(id) ((vv_quirk_detect >> VV_Q_##id) & 1u)
#define ANY(id) ((vv_quirk_active() >> VV_Q_##id) & 1u)

uint32_t vv_quirk_default(void) {
    uint32_t m = 0;
    for (int i = 0; i < VV_Q_COUNT; i++) if (qdef[i]) m |= 1u << i;
    return m;
}

uint32_t vv_quirk_bugs(void) {
    uint32_t m = 0;
    for (int i = 0; i < VV_Q_COUNT; i++) if (qbug[i]) m |= 1u << i;
    return m;
}

const char *vv_quirk_names(void) {
    static char buf[512];
    if (!buf[0]) for (int i = 0; i < VV_Q_COUNT; i++) { if (i) strcat(buf, ","); strcat(buf, qname[i]); }
    return buf;
}
const char *vv_quirk_set_str(uint32_t m) {
    static char buf[512]; buf[0] = 0;
    for (int i = 0; i < VV_Q_COUNT; i++) if (m >> i & 1) { if (buf[0]) strcat(buf, ","); strcat(buf, qname[i]); }
    return buf[0] ? buf : "none";
}
void vv_quirk_list(FILE *f) {
    fprintf(f, "%-15s %-7s %s\n", "quirk", "default", "hardware behaviour (emulated when enabled; 'bug' = guest-bug detector, warns by default)");
    for (int i = 0; i < VV_Q_COUNT; i++) fprintf(f, "%-15s %-7s %s\n", qname[i], qbug[i] ? "bug" : qdef[i] ? "on" : "off", qdesc[i]);
}

static int lookup(const char *t) {
    if (!strcmp(t, "uart-lane")) return VV_Q_MMIO_LANE;        /* old name */
    for (int i = 0; i < VV_Q_COUNT; i++) if (!strcmp(t, qname[i])) return i;
    return -1;
}

/* A list edits `base` left to right: NAME / +NAME add, -NAME / no-NAME remove, and the keywords
 * none / all / default (= `dflt`) replace the set so far.  quiet: don't print errors (validation). */
static int parse_list(const char *list, uint32_t base, uint32_t dflt, uint32_t *out, int quiet) {
    if (!list || !*list) { *out = base; return 0; }
    char *s = strdup(list), *save = NULL;
    uint32_t m = base;
    for (char *t = strtok_r(s, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        int neg = t[0] == '-'; if (neg || t[0] == '+') t++;
        else if (!strncmp(t, "no-", 3)) { neg = 1; t += 3; }
        uint32_t bits;
        if (!strcmp(t, "all")) bits = ALL;
        else if (!strcmp(t, "default")) bits = dflt;
        else if (!strcmp(t, "none")) { m = 0; continue; }
        else { int k = lookup(t);
            if (k < 0) { if (!quiet) vv_eprintf("unknown quirk '%s' (known: all,default,none,%s)\n", t, vv_quirk_names());
                         free(s); return -1; }
            m = neg ? (m & ~(1u << k)) : (m | 1u << k); continue; }
        m = neg ? (m & ~bits) : bits;        /* keywords set the whole set; -all / no-default remove */
    }
    free(s);
    *out = m; return 0;
}
int vv_quirk_parse(const char *list, uint32_t base, uint32_t dflt, uint32_t *out) { return parse_list(list, base, dflt, out, 0); }
int vv_quirk_list_ok(const char *list) { uint32_t m; return list && *list && !parse_list(list, 0, 0, &m, 1); }

/* ---------------- tiny MIPS disassembler (for the diagnostic only) ---------------- */
static const char *R[32] = {"zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
static void disasm(uint32_t pc, uint32_t i, char *o, size_t n) {
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31, sa = (i >> 6) & 31, fn = i & 63;
    int16_t imm = (int16_t)(i & 0xFFFF); uint32_t bt = pc + 4 + ((int32_t)imm << 2);
    static const char *rfn[64] = {[0]="sll",[2]="srl",[3]="sra",[4]="sllv",[6]="srlv",[7]="srav",[8]="jr",[9]="jalr",
        [10]="movz",[11]="movn",[12]="syscall",[13]="break",[15]="sync",[16]="mfhi",[17]="mthi",[18]="mflo",[19]="mtlo",
        [24]="mult",[25]="multu",[26]="div",[27]="divu",[32]="add",[33]="addu",[34]="sub",[35]="subu",[36]="and",
        [37]="or",[38]="xor",[39]="nor",[42]="slt",[43]="sltu",[48]="tge",[49]="tgeu",[50]="tlt",[51]="tltu",[52]="teq",[54]="tne"};
    static const char *iop[64] = {[4]="beq",[5]="bne",[6]="blez",[7]="bgtz",[8]="addi",[9]="addiu",[10]="slti",[11]="sltiu",
        [12]="andi",[13]="ori",[14]="xori",[15]="lui",[20]="beql",[21]="bnel",[22]="blezl",[23]="bgtzl",
        [32]="lb",[33]="lh",[34]="lwl",[35]="lw",[36]="lbu",[37]="lhu",[38]="lwr",[39]="op27",
        [40]="sb",[41]="sh",[42]="swl",[43]="sw",[44]="op2c",[45]="op2d",[46]="swr",[47]="cache",[48]="ll",[56]="sc"};
    static const char *rim[32] = {[0]="bltz",[1]="bgez",[2]="bltzl",[3]="bgezl",[8]="tgei",[9]="tgeiu",[10]="tlti",
        [11]="tltiu",[12]="teqi",[14]="tnei",[16]="bltzal",[17]="bgezal",[18]="bltzall",[19]="bgezall",[31]="synci"};
    if (i == 0) { snprintf(o, n, "nop"); return; }
    if (op == 0) {
        const char *m = rfn[fn]; if (!m) { snprintf(o, n, "special fn=0x%02x", fn); return; }
        if (fn <= 3) snprintf(o, n, "%s %s,%s,%u", (fn == 2 && (rs & 1)) ? "rotr" : m, R[rd], R[rt], sa);
        else if (fn <= 7) snprintf(o, n, "%s %s,%s,%s", (fn == 6 && (sa & 1)) ? "rotrv" : m, R[rd], R[rt], R[rs]);
        else if (fn == 8) snprintf(o, n, "jr %s", R[rs]);
        else if (fn == 9) snprintf(o, n, "jalr %s,%s", R[rd], R[rs]);
        else if (fn == 12 || fn == 13 || fn == 15) snprintf(o, n, "%s", m);
        else if (fn == 16 || fn == 18) snprintf(o, n, "%s %s", m, R[rd]);
        else if (fn == 17 || fn == 19) snprintf(o, n, "%s %s", m, R[rs]);
        else if ((fn >= 24 && fn <= 27) || fn >= 48) snprintf(o, n, "%s %s,%s", m, R[rs], R[rt]);
        else snprintf(o, n, "%s %s,%s,%s", m, R[rd], R[rs], R[rt]);
    } else if (op == 1) {
        if (rim[rt]) snprintf(o, n, "%s %s,0x%08x", rim[rt], R[rs], bt); else snprintf(o, n, "regimm rt=%u", rt);
    } else if (op == 2 || op == 3) snprintf(o, n, "%s 0x%08x", op == 2 ? "j" : "jal", ((pc + 4) & 0xF0000000u) | ((i & 0x3FFFFFF) << 2));
    else if (op == 0x10) {
        if (rs == 0 || rs == 4) snprintf(o, n, "%s %s,$%u%s", rs ? "mtc0" : "mfc0", R[rt], rd, (i & 7) ? ",sel" : "");
        else if (i == 0x42000018) snprintf(o, n, "eret");
        else snprintf(o, n, "cop0 rs=%u fn=0x%02x", rs, fn);
    }
    else if (!iop[op]) snprintf(o, n, "op=0x%02x", op);
    else if (op <= 5 || op == 20 || op == 21) snprintf(o, n, "%s %s,%s,0x%08x", iop[op], R[rs], R[rt], bt);
    else if (op == 6 || op == 7 || op == 22 || op == 23) snprintf(o, n, "%s %s,0x%08x", iop[op], R[rs], bt);
    else if (op >= 32) snprintf(o, n, "%s %s,%d(%s)", iop[op], R[rt], imm, R[rs]);
    else if (op == 15) snprintf(o, n, "lui %s,0x%x", R[rt], i & 0xFFFF);
    else if (op >= 12) snprintf(o, n, "%s %s,%s,0x%x", iop[op], R[rt], R[rs], i & 0xFFFF);
    else snprintf(o, n, "%s %s,%s,%d", iop[op], R[rt], R[rs], imm);
}

/* ---------------- reporting ---------------- */
#define SEEN_N 4096
static uint64_t seen[SEEN_N];            /* (key+1)<<32 | pc, open addressing; dedupe */
static int seen_add(int key, uint32_t pc) {
    uint64_t k = ((uint64_t)(key + 1) << 32) | pc;
    for (uint32_t h = (pc * 2654435761u ^ key) % SEEN_N, n = 0; n < SEEN_N; h = (h + 1) % SEEN_N, n++) {
        if (seen[h] == k) return 0;
        if (!seen[h]) { seen[h] = k; return 1; }
    }
    return 0;                             /* table full: stop logging new sites */
}
static uc_engine *g_uc;
static uint32_t fetch(uc_engine *uc, uint32_t a);
static unsigned rep_back;               /* 1 when reporting the PREVIOUS insn (post-check) */
static void report(int cat, uint32_t pc, uint32_t insn, const char *detail) {
    int halt = (vv_quirk_halt >> cat) & 1;
    /* warnings: once per (category, pc); rx-overrun happens between instructions: once per run */
    if (!halt && !seen_add(cat, cat == VV_Q_RX_OVERRUN ? 0 : pc)) return;
    char d[64]; disasm(pc, insn, d, sizeof d);
    vv_eprintf("[quirk] %s at pc=0x%08x insn=%08x  %-28s %s (insn #%llu)%s\n", qname[cat], pc, insn, d, detail,
            (unsigned long long)(vv_insns() - rep_back),
            halt ? "" : qbug[cat] ? " [guest bug]" : vv_quirk_on(cat) ? " [emulated]" : " [not emulated: standard MIPS]");
    if (!halt) return;
    vv_eprintf("[quirk] HW: %s\n", qdesc[cat]);
    vv_dump_regs(g_uc);
    vv_eprintf("[stop] --halt-on-quirk: %s (use --halt-on-quirk=-%s to skip this category%s)\n", qname[cat], qname[cat],
            qbug[cat] ? "; it still warns" : "");
    vv_quirk_halted = 1;
    uc_emu_stop(g_uc);
}
/* A quirk event: count it, report it if detected. Returns 1 if the run halted (caller returns). */
static int ev(int cat, uint32_t pc, uint32_t insn, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int ev(int cat, uint32_t pc, uint32_t insn, const char *fmt, ...) {
    qhits[cat]++;
    if (!((vv_quirk_detect >> cat) & 1) || vv_quirk_halted) return vv_quirk_halted;
    char detail[256]; va_list ap; va_start(ap, fmt); vsnprintf(detail, sizeof detail, fmt, ap); va_end(ap);
    report(cat, pc, insn, detail);
    return vv_quirk_halted;
}
int vv_quirk_report(int cat, uint32_t pc, const char *fmt, ...) {
    qhits[cat]++;
    if (!((vv_quirk_detect >> cat) & 1) || vv_quirk_halted) return vv_quirk_halted;
    char detail[320]; va_list ap; va_start(ap, fmt); vsnprintf(detail, sizeof detail, fmt, ap); va_end(ap);
    report(cat, pc, fetch(g_uc, pc), detail);
    return vv_quirk_halted;
}
/* An enabled quirk that vvizard cannot reproduce at this site: say so once per (quirk, pc). */
static void not_emulated(int cat, uint32_t pc, uint32_t insn, const char *why) {
    qnoemu[cat]++;
    if (!seen_add(cat + 32, pc)) return;
    char d[64]; disasm(pc, insn, d, sizeof d);
    vv_eprintf("[quirk] %s at pc=0x%08x insn=%08x  %-28s NOT emulated (%s); standard MIPS result used\n",
            qname[cat], pc, insn, d, why);
}
void vv_quirk_summary(void) {
    int any = 0;
    for (int i = 0; i < VV_Q_COUNT; i++) if (qhits[i] || qnoemu[i]) {
        if (!any++) vv_eprintf("[quirk] summary (dynamic hits):");
        vv_eprintf(" %s=%llu", qname[i], (unsigned long long)qhits[i]);
        if (qnoemu[i]) vv_eprintf("(%llu not emulated)", (unsigned long long)qnoemu[i]);
    }
    if (any) vv_eprintf("\n"); else vv_eprintf("[quirk] summary: no quirks hit\n");
}

/* ---------------- helpers ---------------- */
static uint32_t gr(uc_engine *uc, unsigned r) { uint32_t v = 0; if (r) uc_reg_read(uc, UC_MIPS_REG_0 + r, &v); return v; }
static void sr(uc_engine *uc, unsigned r, uint32_t v) { if (r) uc_reg_write(uc, UC_MIPS_REG_0 + r, &v); }
static uint32_t fetch(uc_engine *uc, uint32_t a) {
    unsigned char b[4]; uint32_t c = vv_canon(a);
    /* undecoded / MMIO: HW fetches 0 (and reading MMIO here would have side effects) */
    if (c == 0xFFFFFFFFu || (c >> 28) == 0x8 || uc_mem_read(uc, c, b, 4)) return 0;
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

/* Registers read by insn i (bitmask of GPRs), for the load-use check. */
static uint32_t reads(uint32_t i) {
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, fn = i & 63;
    uint32_t S = 1u << rs, T = 1u << rt;
    switch (op) {
    case 0:
        if (fn <= 3) return T;                       /* sll/srl/sra */
        if (fn == 8 || fn == 9 || fn == 17 || fn == 19) return S;
        if (fn == 12 || fn == 13 || fn == 16 || fn == 18) return 0;
        return S | T;
    case 1: case 6: case 7: return S;
    case 2: case 3: case 15: return 0;
    case 4: case 5: return S | T;
    case 0x10: return rs == 4 ? T : 0;               /* mtc0 reads rt */
    default:
        if (op >= 8 && op <= 14) return S;
        if (op == 0x22 || op == 0x26) return S | T;  /* lwl/lwr merge rt */
        if (op >= 0x20 && op <= 0x27) return S;
        if (op >= 0x28 && op <= 0x2F) return S | T;
        return 0;
    }
}
/* Registers written by insn i as Unicorn executes it (for the load-use restore). */
static uint32_t writes(uint32_t i) {
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31, fn = i & 63;
    switch (op) {
    case 0:
        if (fn == 8 || fn == 12 || fn == 13 || fn == 17 || fn == 19 || (fn >= 24 && fn <= 27)) return 0;
        return 1u << rd;
    case 1: return (rt & 0x10) ? 1u << 31 : 0;
    case 3: return 1u << 31;
    case 0x10: return rs == 0 ? 1u << rt : 0;
    default:
        if ((op >= 8 && op <= 15) || (op >= 0x20 && op <= 0x27)) return 1u << rt;
        return 0;
    }
}
/* Control-transfer insn as Unicorn executes it (the next insn is its delay slot). */
static int is_cti(uint32_t i) {
    unsigned op = i >> 26, rt = (i >> 16) & 31, fn = i & 63;
    return (op == 0 && (fn == 8 || fn == 9)) || (op == 1 && (rt & 0xC) == 0)
        || (op >= 2 && op <= 7) || (op >= 20 && op <= 23);
}

/* ---------------- per-insn state ---------------- */
static struct {
    int active;
    uint32_t pc, insn;
    int nrest; uint8_t rr[4]; uint32_t rv[4];    /* registers restored after the insn ran */
    int nset;  uint8_t qr[3]; uint32_t qv[3];    /* results written after that */
    int hilo; uint32_t hi, lo;                   /* HI/LO restored (hilo) */
    int mem; uint32_t maddr; uint8_t mbytes[4];  /* RAM word restored (dropped store) */
    int vzero;                                   /* re-zero the VOID page */
    int lbchk; unsigned lb_rt; uint32_t lb_mask; /* lb-sign: check / zero-extend rt */
    int lu; unsigned lu_r; uint32_t lu_new;      /* load-use: give rt its loaded value back */
    int ld; unsigned ld_rt; uint32_t ld_old;     /* this insn is a load (start load-use state) */
} P;
static struct { unsigned rt; uint32_t old, newv, pc; } L;   /* the previous insn was a load */
static int prev_cti;
static struct { int pend; uint32_t ds, target; } B;   /* emulated taken branch: after the delay slot at ds */
static uint32_t q_last_pc = 0xFFFFFFFFu;   /* last instruction decoded by pre() */

static void p_rest(uc_engine *uc, unsigned r) { if (r && P.nrest < 4) { P.rr[P.nrest] = r; P.rv[P.nrest++] = gr(uc, r); P.active = 1; } }
static void p_set(unsigned r, uint32_t v) { if (r && P.nset < 3) { P.qr[P.nset] = r; P.qv[P.nset++] = v; P.active = 1; } }

void vv_quirk_init(uc_engine *uc) { g_uc = uc; }

/* undef-reg: GPRs written since reset (bit r).  $zero is always defined. */
static uint32_t reg_defined = 1;
void vv_quirk_reg_defined(unsigned r) { if (r < 32) reg_defined |= 1u << r; }
/* Registers whose VALUE insn i uses (for undef-reg): reads() minus the data operand of a store
 * (saving a never-written register, e.g. a prologue spill or the ISR stash, is harmless) and minus
 * xor/sub/subu rd,rs,rs whose result does not depend on rs. */
static uint32_t value_reads(uint32_t i) {
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, fn = i & 63;
    if (op >= 0x28 && op <= 0x2E) return 1u << rs;
    if (op == 0 && rs == rt && (fn == 34 || fn == 35 || fn == 38)) return 0;
    return reads(i);
}

void vv_quirk_cancel(void) { L.rt = 0; prev_cti = 0; B.pend = 0; memset(&P, 0, sizeof P); }

static void post_fix(uc_engine *uc);
int vv_quirk_post(uc_engine *uc, uint32_t pc) {
    g_uc = uc;
    if (P.active) post_fix(uc);
    if (B.pend && pc != B.ds) {                      /* (the hook at ds itself runs the delay slot) */
        B.pend = 0;
        if (pc == B.ds + 4 && !vv_quirk_halted) {      /* delay slot done: take the branch */
            uc_reg_write(uc, UC_MIPS_REG_PC, &B.target); prev_cti = 0; return 1;
        }
    }
    return 0;
}
static void post_fix(uc_engine *uc) {
    for (int k = 0; k < P.nrest; k++) sr(uc, P.rr[k], P.rv[k]);
    for (int k = 0; k < P.nset; k++) sr(uc, P.qr[k], P.qv[k]);
    if (P.hilo) { uc_reg_write(uc, UC_MIPS_REG_HI, &P.hi); uc_reg_write(uc, UC_MIPS_REG_LO, &P.lo); }
    if (P.mem) uc_mem_write(uc, P.maddr, P.mbytes, 4);
    if (P.vzero) { static const uint8_t z[4096]; uc_mem_write(uc, VV_VOID_PA, z, sizeof z); }
    if (P.lbchk) {
        uint32_t v = gr(uc, P.lb_rt);
        if (v & ~P.lb_mask) {                    /* sign bit was set: old hardware zero-extends */
            rep_back = 1;
            int h = ev(VV_Q_LB_SIGN, P.pc, P.insn, "loaded 0x%08x; old HW gives 0x%08x", v, v & P.lb_mask);
            rep_back = 0;
            if (h) return;
            if (E(LB_SIGN)) sr(uc, P.lb_rt, v & P.lb_mask);
        }
    }
    if (P.lu && !((writes(P.insn) >> P.lu_r) & 1)) sr(uc, P.lu_r, P.lu_new);
    if (P.ld) { L.rt = P.ld_rt; L.old = P.ld_old; L.newv = gr(uc, P.ld_rt); L.pc = P.pc; }
    memset(&P, 0, sizeof P);
}

/* The hardware does nothing for this insn: skip it (PC := pc+4). Not possible in a delay slot. */
static int skip_insn(uc_engine *uc, int cat, uint32_t pc, uint32_t i, int in_ds) {
    if (in_ds) { not_emulated(cat, pc, i, "in a branch delay slot"); return 0; }
    if (P.lu) { sr(uc, P.lu_r, P.lu_new); P.lu = 0; }   /* the insn does not read anything now */
    P.active = 0;
    uint32_t npc = pc + 4; uc_reg_write(uc, UC_MIPS_REG_PC, &npc);
    prev_cti = 0;
    return 1;
}
/* Point the base register at the VOID page so the access cannot fault; restored afterwards. */
static int redirect(uc_engine *uc, int cat, uint32_t pc, uint32_t i, unsigned rs, int16_t imm) {
    if (!rs) { not_emulated(cat, pc, i, "base register is $zero"); return 0; }
    p_rest(uc, rs);
    sr(uc, rs, VV_VOID_VA - (uint32_t)(int32_t)imm);
    return 1;
}

static int known_special(unsigned fn) {
    static const uint64_t ok = (1ull<<0)|(1ull<<2)|(1ull<<3)|(1ull<<4)|(1ull<<6)|(1ull<<7)|(1ull<<8)|(1ull<<9)|
        (1ull<<12)|(1ull<<13)|(0xFull<<16)|(0xFull<<24)|(0xFFull<<32)|(1ull<<42)|(1ull<<43);
    return (ok >> fn) & 1;
}

void vv_quirk_pre(uc_engine *uc, uint32_t pc) {
    g_uc = uc; q_last_pc = pc;
    if (vv_quirk_halted) return;
    uint32_t i = fetch(uc, pc);
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31, sa = (i >> 6) & 31, fn = i & 63;
    int16_t imm = (int16_t)(i & 0xFFFF);
    int in_ds = prev_cti; prev_cti = is_cti(i);
    memset(&P, 0, sizeof P); P.pc = pc; P.insn = i;

    /* undef-reg: reading a register no instruction has written since reset */
    uint32_t und = value_reads(i) & ~reg_defined;
    if (und) {
        unsigned r = (unsigned)__builtin_ctz(und);
        reg_defined |= und;                              /* each register is reported once */
        if (ev(VV_Q_UNDEF_REG, pc, i, "reads $%s, never written since reset (HW: 0 after FPGA configuration, the old value after a reset button press)%s",
               R[r], und & ~(1u << r) ? " (and more)" : "")) return;
    }
    reg_defined |= writes(i);

    /* load-use: this insn reads the rt of the load right before it */
    if (L.rt) {
        unsigned r = L.rt; L.rt = 0;
        if ((reads(i) >> r) & 1) {
            if (ANY(LOAD_USE) && ev(VV_Q_LOAD_USE, pc, i, "reads $%s loaded by insn at 0x%08x (HW sees old 0x%08x, not 0x%08x)",
                                    R[r], L.pc, L.old, L.newv)) return;
            if (E(LOAD_USE)) { sr(uc, r, L.old); P.lu = 1; P.lu_r = r; P.lu_new = L.newv; P.active = 1; }
        }
    }
    uint32_t ld_old = (P.lu && rt == P.lu_r) ? P.lu_new : gr(uc, rt);   /* rt before this insn */

    if (op == 0) {
        if (fn == 9) {                                   /* jalr */
            if (rd != 31 && ANY(JALR_RD)) {
                if (ev(VV_Q_JALR_RD, pc, i, "links to $%s; old HW links to $ra", R[rd])) return;
                if (E(JALR_RD)) { if (rd) p_rest(uc, rd); p_set(31, pc + 8); }
            }
        } else if (fn == 12) {                           /* syscall */
            if (ANY(SYSCALL) && ev(VV_Q_SYSCALL, pc, i, "HW: nop")) return;
            if (E(SYSCALL)) skip_insn(uc, VV_Q_SYSCALL, pc, i, in_ds);
        } else if (fn == 16 || fn == 18) {               /* mfhi/mflo */
            if (ANY(HILO) && ev(VV_Q_HILO, pc, i, "HW writes 0 to $%s", R[rd])) return;
            if (E(HILO)) p_set(rd, 0);
        } else if (fn == 17 || fn == 19 || (fn >= 24 && fn <= 27)) {   /* mthi/mtlo/mult/div */
            if (ANY(HILO) && ev(VV_Q_HILO, pc, i, "HW: no-op")) return;
            if (E(HILO)) { P.hilo = 1; P.active = 1; uc_reg_read(uc, UC_MIPS_REG_HI, &P.hi); uc_reg_read(uc, UC_MIPS_REG_LO, &P.lo); }
        } else if (fn == 32 || fn == 34) {               /* add/sub */
            if (ANY(OVERFLOW)) {
                int32_t a = gr(uc, rs), b = gr(uc, rt), r;
                int ov = fn == 32 ? __builtin_add_overflow(a, b, &r) : __builtin_sub_overflow(a, b, &r);
                uint32_t w = fn == 32 ? (uint32_t)a + (uint32_t)b : (uint32_t)a - (uint32_t)b;
                if (ov) {
                    if (ev(VV_Q_OVERFLOW, pc, i, "0x%08x %c 0x%08x overflows; HW wraps to 0x%08x", a, fn == 32 ? '+' : '-', b, w)) return;
                    if (E(OVERFLOW)) { p_rest(uc, rs); p_rest(uc, rt); sr(uc, rs, 0); sr(uc, rt, 0); p_set(rd, w); }
                }
            }
        } else if (fn == 2 && (rs & 1)) {                /* rotr: HW srl */
            uint32_t w = gr(uc, rt) >> sa;
            if (ANY(RESERVED) && ev(VV_Q_RESERVED, pc, i, "HW does srl: 0x%08x", w)) return;
            if (E(RESERVED)) p_set(rd, w);
        } else if (fn == 6 && (sa & 1)) {                /* rotrv: HW srlv */
            uint32_t w = gr(uc, rt) >> (gr(uc, rs) & 31);
            if (ANY(RESERVED) && ev(VV_Q_RESERVED, pc, i, "HW does srlv: 0x%08x", w)) return;
            if (E(RESERVED)) p_set(rd, w);
        } else if (!known_special(fn)) {                 /* movz/movn/sync/teq/...: rd <- 0 */
            if (ANY(RESERVED) && ev(VV_Q_RESERVED, pc, i, "SPECIAL funct 0x%02x: HW writes 0 to $%s", fn, R[rd])) return;
            if (E(RESERVED) && skip_insn(uc, VV_Q_RESERVED, pc, i, in_ds)) sr(uc, rd, 0);
        }
        return;
    }
    if (op == 1) {                                       /* REGIMM */
        int32_t a = gr(uc, rs); int link = (rt >> 4) & 1, taken = (rt & 1) ? a >= 0 : a < 0;
        if (rt == 16 || rt == 17) {
            if (!taken && ANY(BAL_UNTAKEN)) {
                if (ev(VV_Q_BAL_UNTAKEN, pc, i, "not taken; HW leaves $ra = 0x%08x (MIPS32 links 0x%08x)", gr(uc, 31), pc + 8)) return;
                if (E(BAL_UNTAKEN)) p_rest(uc, 31);
            }
        } else if (rt > 1 && ANY(RESERVED)) {
            int likely = rt == 2 || rt == 3 || rt == 18 || rt == 19;
            if (ev(VV_Q_RESERVED, pc, i, "HW: %s%s, %s", (rt & 1) ? "bgez" : "bltz", link ? "al" : "", taken ? "taken" : "not taken")) return;
            if (E(RESERVED)) {
                if (!taken) { if (skip_insn(uc, VV_Q_RESERVED, pc, i, in_ds) && link && !E(BAL_UNTAKEN)) sr(uc, 31, pc + 8); }
                else if (!likely) {                      /* HW: an ordinary taken branch, Unicorn: a trap insn */
                    uint32_t t = pc + 4 + ((uint32_t)(int32_t)imm << 2);
                    if (skip_insn(uc, VV_Q_RESERVED, pc, i, in_ds)) {
                        if (link) sr(uc, 31, pc + 8);
                        B.pend = 1; B.ds = pc + 4; B.target = t; prev_cti = 1;
                    }
                }
                /* taken likely branch: Unicorn does what the hardware does */
            }
        }
        return;
    }
    if (op == 0x10) {                                    /* COP0 */
        if (rs == 0 || rs == 4) {
            int odd = (i & 7) || !(rd == 9 || rd == 11 || rd == 12 || rd == 13 || rd == 14) || (rs == 4 && rd == 14);
            if (odd && ANY(COP0) && ev(VV_Q_COP0, pc, i, "%s $%u%s: HW %s", rs ? "mtc0" : "mfc0", rd, (i & 7) ? " sel!=0" : "",
                                         rs ? "ignores it" : "reads COP0150 register (0 if none)")) return;
            if (E(COP0)) { if (rs == 0) p_set(rt, vv_cop0_read(rd)); else vv_cop0_write(rd, gr(uc, rt)); }
        } else {                                         /* eret, di/ei, tlb*, wait, ...: nop */
            if (i == 0x42000018u && vv_hw_in_isr()) {      /* irq-storm: returning with the cause still pending */
                uint32_t st = vv_hw_status();
                if (!E(COP0)) uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &st);
                vv_hw_isr_exit_check(st, pc, "eret");
                if (vv_quirk_halted) return;
            }
            if (ANY(RESERVED) && ev(VV_Q_RESERVED, pc, i, "COP0 op: HW nop")) return;
            if (E(RESERVED)) skip_insn(uc, VV_Q_RESERVED, pc, i, in_ds);
        }
        return;
    }
    if ((op >= 0x11 && op <= 0x1F) || op >= 0x30) {      /* COP1-3, branch-likely, SPECIAL2/3, LL/SC/LWC/SWC/PREF */
        if (ANY(RESERVED) && ev(VV_Q_RESERVED, pc, i, "HW nop")) return;
        if (E(RESERVED)) skip_insn(uc, VV_Q_RESERVED, pc, i, in_ds);
        return;
    }
    if (op == 8) {                                       /* addi */
        if (ANY(OVERFLOW)) {
            int32_t a = gr(uc, rs), r; uint32_t w = (uint32_t)a + (uint32_t)(int32_t)imm;
            if (__builtin_add_overflow(a, (int32_t)imm, &r)) {
                if (ev(VV_Q_OVERFLOW, pc, i, "0x%08x + %d overflows; HW wraps to 0x%08x", a, imm, w)) return;
                if (E(OVERFLOW)) { p_rest(uc, rs); sr(uc, rs, 0); p_set(rt, w); }
            }
        }
        return;
    }
    if (op < 0x20) return;

    /* ---- loads/stores 0x20..0x2F ---- */
    int store = op >= 0x28;
    if (op == 0x27 || op == 0x2C || op == 0x2D || op == 0x2F) {   /* decoded as lw/sb/sh/sw, address = ALU_XXX = 0 */
        if (ANY(RESERVED) && ev(VV_Q_RESERVED, pc, i, "HW %s address 0 (%s)", store ? "stores to" : "loads from",
                                store ? "dropped" : "reads 0")) return;
        if (E(RESERVED) && skip_insn(uc, VV_Q_RESERVED, pc, i, in_ds) && !store && rt) {
            sr(uc, rt, 0); L.rt = rt; L.old = ld_old; L.newv = 0; L.pc = pc;   /* still a load (no interlock) */
        }
        return;
    }
    uint32_t a = gr(uc, rs) + (uint32_t)(int32_t)imm, c = vv_canon(a);
    unsigned size;
    switch (op & 7) { case 0: case 4: size = 1; break; case 1: case 5: size = 2; break; case 3: size = 4; break; default: size = 0; }
    int mmio = c != 0xFFFFFFFFu && (c >> 28) == 0x8;
    if (size == 0) {                                     /* lwl/lwr/swl/swr */
        if (ANY(LWLR) && ev(VV_Q_LWLR, pc, i, "addr=0x%08x; HW %s", a, store ? "stores nothing" : "writes 0 to rt")) return;
        if (E(LWLR) && redirect(uc, VV_Q_LWLR, pc, i, rs, imm)) {
            if (mmio) vv_mmio_side_read(c);
            if (store) { P.vzero = 1; P.active = 1; } else p_set(rt, 0);
        }
    } else if (size > 1 && (a & (size - 1))) {          /* unaligned */
        if (ANY(UNALIGNED) && ev(VV_Q_UNALIGNED, pc, i, "addr=0x%08x size %u; HW %s", a, size, store ? "drops the store" : "loads 0")) return;
        if (E(UNALIGNED) && redirect(uc, VV_Q_UNALIGNED, pc, i, rs, imm)) {
            if (mmio) vv_mmio_side_read(c);
            if (store) { P.vzero = 1; P.active = 1; } else p_set(rt, 0);
        }
    } else if (c == 0xFFFFFFFFu) {                       /* undecoded nibble: VOID page via tlb_fill */
        if (ANY(UNMAPPED) && ev(VV_Q_UNMAPPED, pc, i, "%s of undecoded 0x%08x; HW %s", store ? "store" : "load", a,
                                store ? "drops it" : "reads 0")) return;
        if (E(UNMAPPED) && store) { P.vzero = 1; P.active = 1; }
    } else if ((a >> 28) == 0x2 && (store ? (pc >> 28) != 0x4 : 1)) {   /* I-cache path */
        if (ANY(DDR2) && ev(VV_Q_DDR2, pc, i, "%s 0x%08x; HW %s", store ? "store to" : "load from", a,
                            store ? "drops it (not running from BIOS)" : "returns stale D-cache data")) return;
        if (E(DDR2)) {
            if (store) { P.mem = 1; P.active = 1; P.maddr = c & ~3u; uc_mem_read(uc, P.maddr, P.mbytes, 4); }
            else not_emulated(VV_Q_DDR2, pc, i, "load via the I-cache path returns DDR data");
        }
    } else if (store && (c >> 28) == 0x4) {              /* BIOS store */
        if (ANY(BIOS_STORE) && ev(VV_Q_BIOS_STORE, pc, i, "store to 0x%08x (val 0x%08x); HW drops it", a, gr(uc, rt))) return;
        if (E(BIOS_STORE)) { P.mem = 1; P.active = 1; P.maddr = c & ~3u; uc_mem_read(uc, P.maddr, P.mbytes, 4); }
    } else if (store && mmio && size < 4 && (a & 3) + size != 4) {
        uint32_t v = gr(uc, rt) & ((1u << (8 * size)) - 1), placed = v << (8 * (4 - size - (a & 3)));
        if (ANY(MMIO_LANE) && ev(VV_Q_MMIO_LANE, pc, i, "%s to MMIO 0x%08x (val 0x%x); HW delivers 0x%08x%s", size == 1 ? "sb" : "sh",
                                 a, v, placed, (c & VV_MMIO_DECODE) == VV_OW_UATX_DATA ? ", UART sends 0x00" :
                                 ((c & VV_MMIO_DECODE) & ~0x7Fu) == VV_OW_LCD_BASE ? ", LCD char gets 0x00" : "")) return;
        /* emulated in main.c mmio_write() */
    }
    /* D-cache path (nibbles 0x1/0x3), aligned: dcache-stale */
    if (size && !(a & (size - 1)) && c != 0xFFFFFFFFu && (c >> 28) == 0x1 && ((a >> 28) == 0x1 || (a >> 28) == 0x3)) {
        if (store) vv_dc_store(c, size, gr(uc, rt));
        else {
            uint32_t old, cur; uint8_t b[4];
            if (vv_dc_load(c & ~3u, &old) && !uc_mem_read(uc, c & ~3u, b, 4) &&
                old != (cur = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3])) {
                unsigned sh = 8 * (4 - size - (a & 3));
                uint32_t v = size == 4 ? old : (old >> sh) & ((1u << (8 * size)) - 1);
                if (op == 0x20 && (v & 0x80u) && !E(LB_SIGN)) v |= 0xFFFFFF00u;          /* lb  */
                if (op == 0x21 && (v & 0x8000u) && !E(LB_SIGN)) v |= 0xFFFF0000u;        /* lh  */
                if (ev(VV_Q_DCACHE_STALE, pc, i, "load of 0x%08x hits a D-cache line the GPU wrote behind the cache: HW returns the cached 0x%08x (DDR has 0x%08x)",
                       a, old, cur)) return;
                if (E(DCACHE_STALE) && rt) p_set(rt, v);
            }
        }
    }
    if (!store && (op == 0x20 || op == 0x21) && ANY(LB_SIGN) && rt) {
        P.lbchk = 1; P.lb_rt = rt; P.lb_mask = op == 0x20 ? 0xFFu : 0xFFFFu; P.active = 1;
    }
    if (!store && rt) { P.ld = 1; P.ld_rt = rt; P.ld_old = ld_old; P.active = 1; }
}

void vv_quirk_fetch(uc_engine *uc, uint32_t va, const char *why) {
    g_uc = uc;
    if (vv_quirk_halted) return;
    ev(VV_Q_UNMAPPED_FETCH, va, 0, "instruction fetch from %s 0x%08x; HW fetches 0 (nop); last insn executed at 0x%08x",
       why, va, q_last_pc);
}
