/* quirk.c - detect guest code that relies on behaviour where the MIPS150 hardware and
 * standard MIPS (Unicorn) disagree.  See quirk.h for the categories and HW references.
 * Cost when off: one branch in the code hook.  When on: one 4-byte uc_mem_read + decode
 * per instruction, plus GPR reads only for the instruction classes being checked. */
#include "quirk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t vv_quirk_mask;
int vv_quirk_warn, vv_quirk_halted;

static const char *qname[VV_Q_COUNT] = {
#define Q(id, n, d) n,
    VV_QUIRK_TABLE
#undef Q
};
static const char *qdesc[VV_Q_COUNT] = {
#define Q(id, n, d) d,
    VV_QUIRK_TABLE
#undef Q
};
static uint64_t qhits[VV_Q_COUNT];
#define ALL ((1u << VV_Q_COUNT) - 1)
#define ON(id) (vv_quirk_mask & (1u << VV_Q_##id))

const char *vv_quirk_names(void) {
    static char buf[256];
    if (!buf[0]) for (int i = 0; i < VV_Q_COUNT; i++) { if (i) strcat(buf, ","); strcat(buf, qname[i]); }
    return buf;
}

int vv_quirk_parse(const char *list) {
    if (!list || !*list) { vv_quirk_mask = ALL; return 0; }
    char *s = strdup(list), *save = NULL;
    uint32_t m = (list[0] == '-') ? ALL : 0;
    for (char *t = strtok_r(s, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        int neg = t[0] == '-'; if (neg) t++;
        uint32_t bit = 0;
        if (!strcmp(t, "all")) bit = ALL;
        else for (int i = 0; i < VV_Q_COUNT; i++) if (!strcmp(t, qname[i])) bit = 1u << i;
        if (!bit) { fprintf(stderr, "unknown quirk '%s' (known: all,%s)\n", t, vv_quirk_names()); free(s); return -1; }
        m = neg ? (m & ~bit) : (m | bit);
    }
    free(s);
    vv_quirk_mask = m; return 0;
}

/* ---------------- tiny MIPS I disassembler (for the diagnostic only) ---------------- */
static const char *R[32] = {"zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
static void disasm(uint32_t pc, uint32_t i, char *o, size_t n) {
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31, sa = (i >> 6) & 31, fn = i & 63;
    int16_t imm = (int16_t)(i & 0xFFFF); uint32_t bt = pc + 4 + ((int32_t)imm << 2);
    static const char *rfn[64] = {[0]="sll",[2]="srl",[3]="sra",[4]="sllv",[6]="srlv",[7]="srav",[8]="jr",[9]="jalr",
        [12]="syscall",[13]="break",[16]="mfhi",[17]="mthi",[18]="mflo",[19]="mtlo",[24]="mult",[25]="multu",[26]="div",
        [27]="divu",[32]="add",[33]="addu",[34]="sub",[35]="subu",[36]="and",[37]="or",[38]="xor",[39]="nor",[42]="slt",[43]="sltu"};
    static const char *iop[64] = {[4]="beq",[5]="bne",[6]="blez",[7]="bgtz",[8]="addi",[9]="addiu",[10]="slti",[11]="sltiu",
        [12]="andi",[13]="ori",[14]="xori",[15]="lui",[32]="lb",[33]="lh",[34]="lwl",[35]="lw",[36]="lbu",[37]="lhu",[38]="lwr",
        [40]="sb",[41]="sh",[42]="swl",[43]="sw",[46]="swr"};
    if (i == 0) { snprintf(o, n, "nop"); return; }
    if (op == 0) {
        const char *m = rfn[fn]; if (!m) { snprintf(o, n, "special fn=0x%02x", fn); return; }
        if (fn <= 3) snprintf(o, n, "%s %s,%s,%u", m, R[rd], R[rt], sa);
        else if (fn <= 7) snprintf(o, n, "%s %s,%s,%s", m, R[rd], R[rt], R[rs]);
        else if (fn == 8) snprintf(o, n, "jr %s", R[rs]);
        else if (fn == 9) snprintf(o, n, "jalr %s,%s", R[rd], R[rs]);
        else if (fn == 12 || fn == 13) snprintf(o, n, "%s", m);
        else if (fn == 16 || fn == 18) snprintf(o, n, "%s %s", m, R[rd]);
        else if (fn == 17 || fn == 19) snprintf(o, n, "%s %s", m, R[rs]);
        else if (fn >= 24 && fn <= 27) snprintf(o, n, "%s %s,%s", m, R[rs], R[rt]);
        else snprintf(o, n, "%s %s,%s,%s", m, R[rd], R[rs], R[rt]);
    } else if (op == 1) snprintf(o, n, "%s %s,0x%08x", (const char *[]){"bltz","bgez"}[rt & 1], R[rs], bt);
    else if (op == 2 || op == 3) snprintf(o, n, "%s 0x%08x", op == 2 ? "j" : "jal", ((pc + 4) & 0xF0000000u) | ((i & 0x3FFFFFF) << 2));
    else if (op == 0x10) snprintf(o, n, "%s %s,$%u", rs == 0 ? "mfc0" : rs == 4 ? "mtc0" : "cop0?", R[rt], rd);
    else if (!iop[op]) snprintf(o, n, "op=0x%02x", op);
    else if (op <= 5) snprintf(o, n, "%s %s,%s,0x%08x", iop[op], R[rs], R[rt], bt);
    else if (op <= 7) snprintf(o, n, "%s %s,0x%08x", iop[op], R[rs], bt);
    else if (op >= 32) snprintf(o, n, "%s %s,%d(%s)", iop[op], R[rt], imm, R[rs]);
    else if (op == 15) snprintf(o, n, "lui %s,0x%x", R[rt], i & 0xFFFF);
    else if (op >= 12) snprintf(o, n, "%s %s,%s,0x%x", iop[op], R[rt], R[rs], i & 0xFFFF);
    else snprintf(o, n, "%s %s,%s,%d", iop[op], R[rt], R[rs], imm);
}

/* ---------------- reporting ---------------- */
#define SEEN_N 4096
static uint64_t seen[SEEN_N];            /* (cat+1)<<32 | pc, open addressing; warn mode dedupe */
static int seen_add(int cat, uint32_t pc) {
    uint64_t k = ((uint64_t)(cat + 1) << 32) | pc;
    for (uint32_t h = (pc * 2654435761u ^ cat) % SEEN_N, n = 0; n < SEEN_N; h = (h + 1) % SEEN_N, n++) {
        if (seen[h] == k) return 0;
        if (!seen[h]) { seen[h] = k; return 1; }
    }
    return 0;                             /* table full: stop logging new sites */
}
static uc_engine *g_uc;
static unsigned rep_back;               /* 1 when reporting the PREVIOUS insn (post-check) */
static void report(int cat, uint32_t pc, uint32_t insn, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static void report(int cat, uint32_t pc, uint32_t insn, const char *fmt, ...) {
    qhits[cat]++;
    if (vv_quirk_halted) return;
    if (vv_quirk_warn && !seen_add(cat, pc)) return;
    char d[64], detail[256]; va_list ap; va_start(ap, fmt); vsnprintf(detail, sizeof detail, fmt, ap); va_end(ap);
    disasm(pc, insn, d, sizeof d);
    fprintf(stderr, "[quirk] %s at pc=0x%08x insn=%08x  %-28s %s (insn #%llu)\n", qname[cat], pc, insn, d, detail,
            (unsigned long long)(vv_insns() - rep_back));
    if (vv_quirk_warn) return;
    fprintf(stderr, "[quirk] HW: %s\n", qdesc[cat]);
    vv_dump_regs(g_uc);
    fprintf(stderr, "[stop] --halt-on-quirk: %s (use --halt-on-quirk=all,-%s to skip this category)\n", qname[cat], qname[cat]);
    vv_quirk_halted = 1;
    uc_emu_stop(g_uc);
}
void vv_quirk_summary(void) {
    int any = 0;
    for (int i = 0; i < VV_Q_COUNT; i++) if (qhits[i]) {
        if (!any++) fprintf(stderr, "[quirk] summary (dynamic hits):");
        fprintf(stderr, " %s=%llu", qname[i], (unsigned long long)qhits[i]);
    }
    if (any) fputc('\n', stderr); else fprintf(stderr, "[quirk] summary: no quirks hit\n");
}

/* ---------------- detection ---------------- */
static uint32_t gr(uc_engine *uc, unsigned r) { uint32_t v = 0; if (r) uc_reg_read(uc, UC_MIPS_REG_0 + r, &v); return v; }
static uint32_t fetch(uc_engine *uc, uint32_t a) {
    unsigned char b[4]; uint32_t c = vv_canon(a);
    if (c == 0xFFFFFFFFu || uc_mem_read(uc, c, b, 4)) return 0;
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

/* pending state from the previous instruction (a load) */
static int ld_rt;                        /* 0 = none */
static uint32_t ld_pc, ld_insn, ld_addr; static unsigned ld_op;
static uint32_t q_last_pc = 0xFFFFFFFFu;   /* last instruction decoded by pre() */

void vv_quirk_cancel(void) { ld_rt = 0; }

void vv_quirk_post(uc_engine *uc, uint32_t pc) {
    /* lb/lh sign extension is intentionally not flagged: the Verilog is being fixed to sign-extend. */
    (void)pc; g_uc = uc;
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
        if (op >= 0x20 && op <= 0x26) return S;
        if (op >= 0x28 && op <= 0x2E) return S | T;
        return 0;
    }
}

void vv_quirk_pre(uc_engine *uc, uint32_t pc) {
    g_uc = uc; q_last_pc = pc;
    uint32_t i = fetch(uc, pc);
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31, fn = i & 63;
    int prev_rt = ld_rt; uint32_t prev_pc = ld_pc;
    ld_rt = 0;
    if (prev_rt && ON(LOAD_USE) && (reads(i) & (1u << prev_rt)) && !vv_quirk_halted)
        report(VV_Q_LOAD_USE, pc, i, "reads $%s loaded by insn at 0x%08x", R[prev_rt], prev_pc);
    if (vv_quirk_halted) return;
    if (op == 0) {
        if (fn == 9 && rd != 31 && ON(JALR_RD))
            report(VV_Q_JALR_RD, pc, i, "links to $%s; HW links to $ra", R[rd]);
        else if (fn == 12 && ON(SYSCALL)) report(VV_Q_SYSCALL, pc, i, "%s", "");
        else if (fn >= 16 && fn <= 27 && ON(HILO)) report(VV_Q_HILO, pc, i, "%s", fn == 16 || fn == 18 ? "HW writes 0 to rd" : "HW: no-op");
        else if ((fn == 32 || fn == 34) && ON(OVERFLOW)) {
            int32_t a = gr(uc, rs), b = gr(uc, rt), r;
            int ov = fn == 32 ? __builtin_add_overflow(a, b, &r) : __builtin_sub_overflow(a, b, &r);
            if (ov) report(VV_Q_OVERFLOW, pc, i, "0x%08x %c 0x%08x overflows; HW wraps to 0x%08x", a, fn == 32 ? '+' : '-', b,
                           fn == 32 ? (uint32_t)a + (uint32_t)b : (uint32_t)a - (uint32_t)b);
        }
        return;
    }
    if (op == 8 && ON(OVERFLOW)) {
        int32_t a = gr(uc, rs), b = (int16_t)(i & 0xFFFF), r;
        if (__builtin_add_overflow(a, b, &r))
            report(VV_Q_OVERFLOW, pc, i, "0x%08x + %d overflows; HW wraps to 0x%08x", a, b, (uint32_t)a + (uint32_t)b);
        return;
    }
    if (op < 0x20 || op > 0x2E) return;
    int store = op >= 0x28;
    if (!store && op > 0x26) return;                 /* 0x27: not MIPS I */
    if (store && (op == 0x2C || op == 0x2D)) return;
    uint32_t a = gr(uc, rs) + (uint32_t)(int32_t)(int16_t)(i & 0xFFFF);
    unsigned w = op & 3;                             /* 0=b 1=h 2=lwl/swl 3=w ; lbu/lhu 4,5 & lwr 6 */
    unsigned size = (op == 0x22 || op == 0x26 || op == 0x2A || op == 0x2E) ? 0 : (w == 0 ? 1 : w == 1 ? 2 : 4);
    if (op == 0x24) size = 1; else if (op == 0x25) size = 2;
    uint32_t c = vv_canon(a);
    if (c == 0xFFFFFFFFu) {
        if (ON(UNMAPPED)) report(VV_Q_UNMAPPED, pc, i, "%s of undecoded 0x%08x; HW %s", store ? "store" : "load", a,
                                 store ? "drops it" : "reads 0");
        return;                                      /* Unicorn would fault next anyway */
    }
    if (store && (c >> 28) == 0x4 && ON(BIOS_STORE))
        report(VV_Q_BIOS_STORE, pc, i, "store to 0x%08x (val 0x%08x); HW drops it", a, gr(uc, rt));
    else if (size == 0 && ON(LWLR)) report(VV_Q_LWLR, pc, i, "addr=0x%08x", a);
    else if (size > 1 && (a & (size - 1)) && ON(UNALIGNED))
        report(VV_Q_UNALIGNED, pc, i, "addr=0x%08x size %u; HW %s", a, size, store ? "drops the store" : "loads 0");
    else if (store && size && size < 4 && (c >> 28) == 0x8 && ((c & 0xFF) & ~3u) == 0x08 && (a & 3) + size != 4 && ON(UART_LANE))
        report(VV_Q_UART_LANE, pc, i, "%s to UART TX 0x%08x (val 0x%02x); HW sends 0x00", size == 1 ? "sb" : "sh", a, gr(uc, rt) & 0xFF);
    if (vv_quirk_halted) return;
    if (!store && rt) { ld_rt = rt; ld_pc = pc; ld_insn = i; ld_op = op; ld_addr = a; }
}

void vv_quirk_fetch(uc_engine *uc, uint32_t va, const char *why) {
    g_uc = uc;
    if (!ON(UNMAPPED) || vv_quirk_halted) return;
    report(VV_Q_UNMAPPED, va, 0, "instruction fetch from %s 0x%08x; HW fetches 0 (nop); last insn executed at 0x%08x",
           why, va, q_last_pc);
}
