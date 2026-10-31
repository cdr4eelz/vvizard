/* VVIZARD - minimal Unicorn-based emulator for the CS150 MIPS150 system.
 * Written from scratch (no code from tool/vizard). */
#include <unicorn/unicorn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <poll.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <time.h>
#include "memmap.h"
#include "loader.h"
#include "display.h"
#include "gpu.h"
#include "quirk.h"
#include "prng.h"
#include "lcd.h"

/* --uart-in STR: bytes fed to UART RX before stdin (e.g. auto-type a GIOS command).
 * Escapes \r \n \t \\ are decoded. GIOS ends tokens on space or CR (not LF). */
static char *uart_in;
static char *decode_escapes(const char *s) {
    char *o = malloc(strlen(s) + 1), *d = o;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *d++ = *s == 'r' ? '\r' : *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
        } else *d++ = *s;
    }
    *d = 0; return o;
}

#ifndef VV_DEFAULT_BIG_ENDIAN
#define VV_DEFAULT_BIG_ENDIAN 1
#endif

static const vv_region regions[] = {
#define X(n, nib, b, s, p, k) { n, nib, b, s, p, k },
    VV_MEMMAP_TABLE
#undef X
};
#define NREG (sizeof regions / sizeof regions[0])
static const struct { uint8_t from, to; } aliases[] = {
#define A(f, t) { f, t },
    VV_ALIAS_TABLE
#undef A
};

static const vv_region *region_for(uint32_t a) {
    uint8_t nib = a >> 28;
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++)
        if (aliases[i].from == nib) nib = aliases[i].to;
    for (size_t i = 0; i < NREG; i++) if (regions[i].nibble == nib) return &regions[i];
    return NULL;
}
/* Reproduce the hardware top-nibble decode: any address in a nibble folds onto
 * base | (addr & (size-1)).  Returns 0xFFFFFFFF for undecoded nibbles. */
uint32_t vv_canon(uint32_t a) {
    const vv_region *r = region_for(a);
    return r ? r->base | (a & (r->size - 1)) : 0xFFFFFFFFu;
}

/* ---------------- state ---------------- */
static int opt_trace, opt_quiet_mmio, opt_trace_regs;
static uint32_t prev_regs[32];
static uint64_t insn_count, cnt_base_insn;
static uint64_t max_insns;
static uint32_t stop_addr; static int have_stop;
static uint32_t pf_frame = 0x10000000u, gp_frame, gp_code, gp_launches;
static int trace_left = -1;

/* ---- UART RX sources: --uart-in, then --input-script bytes that are due, then stdin ---- */
static int stdin_peek = -1, stdin_eof;
static char script_q[4096]; static size_t sq_head, sq_tail;   /* due script bytes */
static int rx_avail(void) {
    if (uart_in && *uart_in) return 1;
    if (sq_head != sq_tail) return 1;
    if (vv_disp_key_avail()) return 1;
    if (stdin_peek < 0 && !stdin_eof) {
        struct pollfd p = { 0, POLLIN, 0 }; unsigned char c;
        if (poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) {
            if (read(0, &c, 1) == 1) stdin_peek = c; else stdin_eof = 1; }
    }
    return stdin_peek >= 0;
}
static int rx_take(void) {
    if (!rx_avail()) return 0;
    if (uart_in && *uart_in) { int c = (unsigned char)*uart_in++; vv_disp_key_log(c); return c; }
    if (sq_head != sq_tail) { int c = (unsigned char)script_q[sq_head]; sq_head = (sq_head + 1) % sizeof script_q; return c; }
    if (vv_disp_key_avail()) return vv_disp_key_take();       /* logged when pressed (display.c) */
    int c = stdin_peek; stdin_peek = -1; vv_disp_key_log(c); return c;
}

/* ---- --input-script FILE: timed events. Line: WHEN ACTION ARG
 *   WHEN   fN (Nth PF_FRAME write) or iN (instruction count N)
 *   ACTION key STR  (queue STR on UART RX; escapes \r \n \t \\ \s=space)
 *          dump PATH (write the displayed frame as PPM at that point)
 * '#' starts a comment. Events must be listed in time order per kind. ---- */
typedef struct { int is_frame; uint64_t when; int is_dump; char *arg; } vv_event;
static vv_event *events; static int nevents, ev_next_f, ev_next_i;
static uint64_t pf_writes;               /* number of PF_FRAME writes so far */
static int opt_irq;                      /* inject UARX interrupts (on with --input-script or --irq) */
static unsigned dump_every; static const char *dump_prefix = "frame";
static void run_event(uc_engine *uc, vv_event *e) {
    if (e->is_dump) vv_disp_dump_ppm(uc, e->arg);
    else for (char *c = e->arg; *c; c++) {
        size_t n = (sq_tail + 1) % sizeof script_q;
        if (n == sq_head) { fprintf(stderr, "[script] RX queue full\n"); break; }
        script_q[sq_tail] = *c; sq_tail = n; vv_disp_key_log((unsigned char)*c);
    }
    fprintf(stderr, "[script] %c%llu %s %s (insn %llu, frame %llu)\n", e->is_frame ? 'f' : 'i',
            (unsigned long long)e->when, e->is_dump ? "dump" : "key", e->is_dump ? e->arg : "", 
            (unsigned long long)insn_count, (unsigned long long)pf_writes);
}
static void run_due_events(uc_engine *uc, int frame_kind) {
    for (int i = 0; i < nevents; i++) {
        vv_event *e = &events[i];
        if (!e->arg || e->is_frame != frame_kind) continue;
        if (e->when <= (frame_kind ? pf_writes : insn_count)) { run_event(uc, e); free(e->arg); e->arg = NULL; }
    }
}
static int load_script(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return -1; }
    char line[1024]; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++; char *h = strchr(line, '#'); if (h) *h = 0;
        char w[64], act[16], arg[900] = "";
        int n = sscanf(line, " %63s %15s %899[^\n]", w, act, arg);
        if (n <= 0) continue;
        if (n < 2 || (w[0] != 'f' && w[0] != 'i') || (strcmp(act, "key") && strcmp(act, "dump"))) {
            fprintf(stderr, "%s:%d: bad line\n", path, ln); fclose(f); return -1; }
        for (char *e = arg + strlen(arg); e > arg && (e[-1] == ' ' || e[-1] == '\r'); ) *--e = 0;
        events = realloc(events, (nevents + 1) * sizeof *events);
        vv_event *e = &events[nevents++];
        e->is_frame = w[0] == 'f'; e->when = strtoull(w + 1, 0, 0); e->is_dump = !strcmp(act, "dump");
        if (e->is_dump) e->arg = strdup(arg);
        else { for (char *q = arg; *q; q++) if (q[0] == '\\' && q[1] == 's') { q[0] = ' '; memmove(q + 1, q + 2, strlen(q + 2) + 1); }
               e->arg = decode_escapes(arg); }
    }
    fclose(f); (void)ev_next_f; (void)ev_next_i; return 0;
}

/* ---- Shadow COP0 Cause/EPC + UARX interrupt injection ----
 * Unicorn 2.1.1 can't set Cause IP bits or read Cause/EPC from the host (NOTES.md), and its
 * exception entry would use the MIPS vector + EXL, which COP0150 lacks.  So: when Status.IE
 * (bit 0) and Status.IM_UARX (bit 10) are set and RX data is available, we emulate the COP0150
 * entry ourselves: EPC=pc, clear Status.IE, jump to PC_ISR (0xC0000180).  mfc0 $13/$14 results
 * are patched with the shadow values; mtc0 $13/$14 update the shadow.  The ISR returns with
 * `jr $k0; mtc0 Status` (no eret), which Unicorn executes natively. */
static uint32_t sh_cause, sh_epc; static int fix_rt = -1; static uint32_t fix_val;
static uint32_t last_pc = 0xFFFFFFFFu; static uint64_t irq_count;
#define VV_IM_UARX 0x400u
static uint32_t read_insn(uc_engine *uc, uint32_t a) {
    unsigned char b[4]; uint32_t c = vv_canon(a);
    if (c == 0xFFFFFFFFu || (c >> 28) == 0x8 || uc_mem_read(uc, c, b, 4)) return 0;   /* no MMIO side effects */
    return b[0]<<24|b[1]<<16|b[2]<<8|b[3];
}
static int is_branch(uint32_t i) {
    unsigned op = i >> 26, fn = i & 0x3F;
    return (op == 0 && (fn == 8 || fn == 9)) || op == 1 || (op >= 2 && op <= 7) || (op >= 20 && op <= 23);
}
/* COP0150 register model for the cop0 quirk (COP0150.v): Count runs (one tick per instruction
 * here), Compare resets to 0xFFFF and a write clears the timer IP bit (Cause[15]), Status is a
 * plain 32-bit register reset to 0, EPC is written only by interrupt entry, everything else reads 0. */
static uint32_t sh_status, sh_compare = 0xFFFFu, cnt0_val; static uint64_t cnt0_insn;
uint32_t vv_cop0_read(unsigned rd) {
    switch (rd) {
    case 9:  return cnt0_val + (uint32_t)(insn_count - cnt0_insn);
    case 11: return sh_compare;
    case 12: return sh_status;
    case 13: return sh_cause | (rx_avail() ? VV_IM_UARX : 0);
    case 14: return sh_epc;
    default: return 0;
    }
}
void vv_cop0_write(unsigned rd, uint32_t v) {
    switch (rd) {
    case 9:  cnt0_val = v; cnt0_insn = insn_count + 1; break;
    case 11: sh_compare = v; sh_cause &= ~0x8000u; break;
    case 12: sh_status = v; break;
    case 13: sh_cause = (v & ~0xFC00u) | (sh_cause & v & 0xFC00u & ~VV_IM_UARX); break;
    default: break;                              /* EPC (14) and the rest: ignored by COP0150 */
    }
}
void vv_mmio_side_read(uint32_t canon) {         /* a HW read of UARX_DATA pops the RX FIFO */
    if ((canon & VV_MMIO_DECODE) == VV_OW_UARX_DATA) (void)rx_take();
}
static void irq_step(uc_engine *uc, uint32_t pc) {
    int cop0 = vv_quirk_on(VV_Q_COP0);
    if (fix_rt > 0) { uc_reg_write(uc, UC_MIPS_REG_0 + fix_rt, &fix_val); }
    fix_rt = -1;
    if (!cop0 && (pc >> 28) == 0xC) {                    /* COP0 shadowing only needed in the ISR BRAM */
        uint32_t i = read_insn(uc, pc);
        if ((i >> 26) == 0x10 && (i & 0x7FF) == 0) {
            unsigned rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31;
            uint32_t v;
            if (rs == 0 && (rd == 13 || rd == 14)) {      /* mfc0 */
                fix_rt = rt; fix_val = rd == 14 ? sh_epc : (sh_cause | (rx_avail() ? VV_IM_UARX : 0));
            } else if (rs == 4 && (rd == 13 || rd == 14)) { /* mtc0 */
                uc_reg_read(uc, UC_MIPS_REG_0 + rt, &v);
                if (rd == 13)   /* COP0150: IP bits 15:10 only clear (next_ip & wd); others written as-is */
                    sh_cause = (v & ~0xFC00u) | (sh_cause & v & 0xFC00u & ~VV_IM_UARX);  /* UARX is level */
                else sh_epc = v;
            }
        }
    }
    if (rx_avail()) {
        uint32_t st; uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &st);
        if (cop0) st = sh_status;
        if ((st & 1) && (st & VV_IM_UARX) && !(last_pc + 4 == pc && is_branch(read_insn(uc, last_pc)))) {
            sh_epc = pc; st &= ~1u; uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &st);
            if (cop0) sh_status = st;
            uint32_t npc = VV_PC_ISR; irq_count++;
            last_pc = 0xFFFFFFFFu;
            uc_reg_write(uc, UC_MIPS_REG_PC, &npc);   /* insn at pc not executed; resumes at ISR */
            return;
        }
    }
    last_pc = pc;
}

static uint64_t mmio_read(uc_engine *uc, uint64_t off, unsigned size, void *u) {
    (void)uc; (void)u; (void)size;
    uint32_t w = off & VV_MMIO_DECODE;          /* addra[11:0]; registers only at addra[11:6]==0 */
    uint32_t v = 0;
    if (w >= VV_MMIO_REG_SPAN) {                /* LCD window reads 0; elsewhere HW reads 32'dx */
        if (!opt_quiet_mmio && (w < VV_OW_LCD_BASE || w >= VV_OW_LCD_END))
            fprintf(stderr, "[mmio] MISS-READ off=0x%04x\n", (unsigned)off);
        w = 0xFFFFFFFFu;                        /* no register: v stays 0 */
    }
    switch (w) {
    case VV_OW_UATX_READY: v = 1; break;        /* host stdout always ready */
    case VV_OW_UARX_VALID: v = rx_avail(); break;
    case VV_OW_UARX_DATA:  v = rx_take(); break;
    case VV_OW_CNT_CYCLE: /* no cycle model: cycles == instructions */
    case VV_OW_CNT_INST:  v = (uint32_t)(insn_count - cnt_base_insn); break;
    case VV_OW_PF_FRAME:  v = 0x10000000u | (((pf_frame >> VV_FSHIFT) & VV_FNMASK) << VV_FSHIFT); break;
    case VV_OW_GP_STATE:  /* graphics.h:72-92 gstate_s: {pf_fault,pf_dormant,pf_frame[5:0], 8'b0,
                           * gp_fault,0,gp_procframe[5:0], 3'b0,rect,elip,line,filler,gp_ready}.
                           * Stub GPU finishes instantly => all engines ready. */
        v = (vv_disp_frame() & 0x3F) << 24
          | (((gp_frame >> 28) ? (gp_frame >> VV_FSHIFT) : gp_frame) & 0x3F) << 16 | 0x1F; break;
    case VV_OW_RANDOM32:  v = vv_prng_read(insn_count); break;  /* cycle == insn count (see prng.c) */
    case 0xFFFFFFFFu: break;
    default:
        if (!opt_quiet_mmio) fprintf(stderr, "[mmio] MISS-READ off=0x%04x\n", (unsigned)off);
    }
    /* Sub-word reads (e.g. lbu OB_UARX_DATA = +3 on big-endian): Unicorn hands us the
     * byte offset; return the matching lane of the 32-bit big-endian word. */
    if (size < 4) { unsigned lane = off & 3; v = (v >> (8 * (4 - size - lane))) & ((1u << (8 * size)) - 1); }
    if (!opt_quiet_mmio && w != VV_OW_UATX_READY && w != VV_OW_UARX_VALID)
        fprintf(stderr, "[mmio] R off=0x%04x size=%u -> 0x%08x\n", (unsigned)off, size, v);
    return v;
}
static void mmio_write(uc_engine *uc, uint64_t off, unsigned size, uint64_t val, void *u) {
    (void)u;
    uint32_t w = off & VV_MMIO_DECODE, v = (uint32_t)val;   /* addra[11:0] */
    /* mmio-lane quirk: ByteAccess4 places sb/sh data in its big-endian byte lane (zeros elsewhere)
     * and MemMapIO uses the whole dina, e.g. sb to UATX_DATA+0 transmits 0x00. */
    int lane_hw = size < 4 && vv_quirk_on(VV_Q_MMIO_LANE);
    if (lane_hw) { unsigned lane = off & 3; v = (v & ((1u << (8 * size)) - 1)) << (8 * (4 - size - lane)); }
    if (w >= VV_MMIO_REG_SPAN) {                /* registers decode only at addra[11:6]==0 */
        if (w < VV_OW_LCD_END) {                /* LCDText2x16_MemPort: addra[11:5]==2, wdata = dina[7:0] */
            vv_lcd_write((w - VV_OW_LCD_BASE) >> 2, (uint8_t)v);
            if (!opt_quiet_mmio) fprintf(stderr, "[mmio] W off=0x%04x size=%u <- 0x%08x (LCD[%u] = 0x%02x)\n",
                                         (unsigned)off, size, v, (unsigned)(w - VV_OW_LCD_BASE) >> 2, v & 0xFF);
        } else if (!opt_quiet_mmio) fprintf(stderr, "[mmio] MISS-WRITE off=0x%04x size=%u <- 0x%08x (dropped)\n", (unsigned)off, size, v);
        return;
    }
    switch (w) {
    case VV_OW_UATX_DATA: putchar(v & 0xFF); fflush(stdout); return; /* dina[7:0] */
    case VV_OW_CNT_RESET: cnt_base_insn = insn_count; break;
    case VV_OW_PF_FRAME:  pf_frame = v; vv_disp_pf_write(uc, v); pf_writes++;
        if (dump_every && pf_writes % dump_every == 0) { char fn[PATH_MAX];
            snprintf(fn, sizeof fn, "%s%06llu.ppm", dump_prefix, (unsigned long long)pf_writes); vv_disp_dump_ppm(uc, fn); }
        if (nevents) run_due_events(uc, 1);
        break;
    case VV_OW_GP_FRAME:  gp_frame = v; break;
    case VV_OW_RANDOM32:  /* any byte mask reseeds with the full dina: ByteAccess4 puts the sb/sh
                           * data in its big-endian lane and zeros the rest (seed 0 -> 1 in RTL) */
        if (size < 4 && !lane_hw) { unsigned lane = off & 3;   /* (already placed when lane_hw) */
            v = (v & ((1u << (8 * size)) - 1)) << (8 * (4 - size - lane)); }
        vv_prng_write(insn_count, v); break;
    case VV_OW_GP_GCODE:  gp_code = v; gp_launches++;   /* write launches GP (MemMapIO.v:50-51) */
        { int n = vv_gpu_run(uc, gp_frame, gp_code, opt_quiet_mmio ? 0 : 1);
          if (!opt_quiet_mmio) fprintf(stderr, "[gpu] launch #%u code@0x%08x frame=0x%08x -> %d cmds\n", gp_launches, gp_code, gp_frame, n); }
        /* HW would raise irq_gp_done (IB_GPU=12) here; interrupts are not injected yet. */
        break;
    default: break;
    }
    if (!opt_quiet_mmio) fprintf(stderr, "[mmio] W off=0x%04x size=%u <- 0x%08x\n", (unsigned)off, size, v);
}

static int stop_reason_intr = -1;
static bool tlb_fill(uc_engine *uc, uint64_t va, uc_mem_type t, uc_tlb_entry *e, void *u) {
    (void)uc; (void)t; (void)u;
    uint32_t pa = vv_canon((uint32_t)va);
    int fetch = t == UC_MEM_FETCH, hole = pa == 0xFFFFFFFFu;
    if (fetch && (hole || (pa >> 28) == 0x8)) {   /* HW fetches DEAD_IMEM = 0 (MemBank.v:132) */
        if ((vv_quirk_active() >> VV_Q_UNMAPPED_FETCH & 1) && stop_reason_intr < 0 && /* not Unicorn's own exception vector */
            !(max_insns && insn_count >= max_insns))
            vv_quirk_fetch(uc, (uint32_t)va, hole ? "undecoded" : "MMIO");
        if (vv_quirk_on(VV_Q_UNMAPPED_FETCH)) {    /* execute nops from the VOID page */
            e->paddr = VV_VOID_PA;
            e->perms = UC_PROT_EXEC | (hole ? UC_PROT_READ | (vv_quirk_on(VV_Q_UNMAPPED) ? UC_PROT_WRITE : 0) : 0);
            return true;
        }
    }
    if (hole) {                                  /* undecoded data access: HW reads 0, drops stores */
        int redir = ((uint32_t)va & ~0xFFFu) == (VV_VOID_VA & ~0xFFFu) &&
                    (vv_quirk_on(VV_Q_UNALIGNED) || vv_quirk_on(VV_Q_LWLR));
        if (!fetch && (redir || vv_quirk_on(VV_Q_UNMAPPED))) {
            e->paddr = VV_VOID_PA;
            e->perms = UC_PROT_READ | UC_PROT_WRITE | (vv_quirk_on(VV_Q_UNMAPPED_FETCH) ? UC_PROT_EXEC : 0);
            return true;
        }
        return false;
    }
    e->paddr = pa & ~0xFFFull;                   /* page granularity; offset kept by Unicorn */
    e->perms = region_for((uint32_t)va)->prot;
    return true;
}

static void hook_code(uc_engine *uc, uint64_t addr, uint32_t size, void *u) {
    (void)size; (void)u;
    insn_count++;
    if (nevents) run_due_events(uc, 0);
    if (vv_quirk_active()) {                 /* emulated quirks and/or --halt-on-quirk / --warn-on-quirk */
        if (vv_quirk_halted) { uc_emu_stop(uc); return; }
        if (vv_quirk_post(uc, (uint32_t)addr)) { insn_count--; return; }   /* fix up the previous insn; emulated branch */
        uint64_t irqs = irq_count;
        if (opt_irq) irq_step(uc, (uint32_t)addr);
        if (irqs != irq_count) vv_quirk_cancel();   /* redirected to the ISR: insn at addr not run */
        else if (!vv_quirk_halted) vv_quirk_pre(uc, (uint32_t)addr);
        if (vv_quirk_halted) return;
    } else if (opt_irq) irq_step(uc, (uint32_t)addr);
    if ((insn_count & 0xFFF) == 0 && vv_disp_tick(uc)) { fprintf(stderr, "[stop] window closed\n"); uc_emu_stop(uc); }
    if (opt_trace && trace_left != 0) {
        uint32_t insn = 0; unsigned char b[4];
        uint32_t ca = vv_canon((uint32_t)addr);
        if (ca != 0xFFFFFFFFu && (ca >> 28) != 0x8 && !uc_mem_read(uc, ca, b, 4)) insn = b[0]<<24|b[1]<<16|b[2]<<8|b[3];
        fprintf(stderr, "[trace] %8llu pc=0x%08x insn=%08x", (unsigned long long)insn_count, (uint32_t)addr, insn);
        if (opt_trace_regs) { /* GPRs changed by the PREVIOUS instruction (debug aid) */
            for (int i = 1; i < 32; i++) { uint32_t v; uc_reg_read(uc, UC_MIPS_REG_0 + i, &v);
                if (v != prev_regs[i]) { fprintf(stderr, " | r%d=%08x", i, v); prev_regs[i] = v; } }
        }
        fputc('\n', stderr);
        if (trace_left > 0) trace_left--;
    }
}
static void hook_intr(uc_engine *uc, uint32_t intno, void *u) {
    (void)u; uint32_t pc; uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
    stop_reason_intr = intno;
    if (vv_quirk_halted) { uc_emu_stop(uc); return; }   /* already reported by --halt-on-quirk */
    /* QEMU-MIPS EXCP_* numbering: 8=EXT_INTERRUPT 12=AdEL 13=AdES 14=TLBF 17=SYSCALL 18=BREAK 20=RI
     * (pc reported here is not reliable inside the hook; Unicorn may show 0). */
    const char *nm = intno == 18 ? "BREAK" : intno == 17 ? "SYSCALL" : intno == 8 ? "EXT_INTERRUPT" :
                     intno == 12 ? "AdEL" : intno == 13 ? "AdES" : intno == 20 ? "RI" : "?";
    fprintf(stderr, "[stop] CPU exception intno=%u (%s) pc=0x%08x\n", intno, nm, pc);
    uc_emu_stop(uc);
}
static bool hook_unmapped(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u) {
    (void)uc; (void)u;
    if (vv_quirk_halted) return false;
    fprintf(stderr, "[fault] unmapped %s at 0x%08x size %d val 0x%llx\n",
            t == UC_MEM_FETCH_UNMAPPED ? "fetch" : t == UC_MEM_WRITE_UNMAPPED ? "write" : "read",
            (uint32_t)a, sz, (long long)v);
    return false;
}
static void hook_bios_write(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u) {
    (void)uc; (void)t; (void)u;
    fprintf(stderr, "[warn] data write to BIOS 0x%08x size %d val 0x%llx (HW drops this; %s)\n", (uint32_t)a, sz, (long long)v,
            vv_quirk_on(VV_Q_BIOS_STORE) ? "dropped: bios-store quirk on" : "kept: bios-store quirk off");
}

/* DDR power-up contents: random by default (real DRAM is not zeroed). splitmix64, seed printed for repeatability. */
static int      opt_random_ddr = 1;
static int      ddr_seed_given = 0;
static uint64_t ddr_seed;

static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int fill_ddr_random(uc_engine *uc, uint32_t base, uint32_t size) {
    enum { CHUNK = 1u << 20 };
    static uint64_t buf[CHUNK / 8];
    uint64_t st = ddr_seed;
    for (uint32_t off = 0; off < size; off += CHUNK) {
        uint32_t n = size - off < CHUNK ? size - off : CHUNK;
        for (uint32_t i = 0; i < (n + 7) / 8; i++) buf[i] = splitmix64(&st);
        uc_err e = uc_mem_write(uc, base + off, buf, n);
        if (e) { fprintf(stderr, "random-ddr @0x%08x: %s\n", base + off, uc_strerror(e)); return 1; }
    }
    return 0;
}

static const char *gpr[32] = {"zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
static void dump_regs(uc_engine *uc) {
    uint32_t v;
    fprintf(stderr, "---- registers (insns=%llu) ----\n", (unsigned long long)insn_count);
    for (int i = 0; i < 32; i++) {
        uc_reg_read(uc, UC_MIPS_REG_0 + i, &v);
        fprintf(stderr, "%4s=%08x%s", gpr[i], v, (i % 4 == 3) ? "\n" : "  ");
    }
    uc_reg_read(uc, UC_MIPS_REG_PC, &v); fprintf(stderr, "  pc=%08x  ", v);
    uc_reg_read(uc, UC_MIPS_REG_HI, &v); fprintf(stderr, "hi=%08x  ", v);
    uc_reg_read(uc, UC_MIPS_REG_LO, &v); fprintf(stderr, "lo=%08x  ", v);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &v); fprintf(stderr, "c0_status=%08x  ", v);
    fprintf(stderr, "\n"); /* Unicorn 2.1 exposes no Cause/EPC/Count reg ids */
}
void vv_dump_regs(uc_engine *uc) { dump_regs(uc); }

/* ---- status strip text (display.c) ---- */
static const char *run_state = "RUN";
static int guest_big = VV_DEFAULT_BIG_ENDIAN;
static uint32_t rd_guest(uc_engine *uc, uint32_t a, unsigned n) {
    unsigned char b[4] = { 0 }; uint32_t v = 0;
    if (uc_mem_read(uc, a, b, n)) return 0;
    for (unsigned i = 0; i < n; i++) v |= (uint32_t)b[guest_big ? i : n - 1 - i] << (8 * (n - 1 - i));
    return v;
}
void vv_status_lines(uc_engine *uc, char *l1, size_t n1, char *l2, size_t n2) {
    snprintf(l1, n1, "%s  insns %llu  PF_FRAME %u (writes %llu)  GP %u  IRQ %llu%s",
             vv_quirk_halted ? "HALTED (quirk)" : run_state, (unsigned long long)insn_count, vv_disp_frame(),
             (unsigned long long)pf_writes, gp_launches, (unsigned long long)irq_count,
             vv_disp_keys_enabled ? "" : "  keys->UART off");
    /* proj6/proj7 shared memory (shared7.h): magic 0xFEDBEEFn, flags (pause / notimer), clock (BCD mm:ss) */
    uint32_t magic = rd_guest(uc, VV_SM_BASE_ADDR, 4);
    if ((magic & ~0xFu) == 0xFEDBEEF0u) {
        uint32_t fl = rd_guest(uc, VV_SM_BASE_ADDR + 0x16, 2), clk = rd_guest(uc, VV_SM_BASE_ADDR + 0x20, 4);
        snprintf(l2, n2, "graphics %s  clock %s  %X%X:%X%X  proj%u", fl & 1 ? "PAUSED" : "running",
                 fl & 2 ? "PAUSED" : "running", clk >> 12 & 15, clk >> 8 & 15, clk >> 4 & 15, clk & 15, 4 + (magic & 0xF));
    } else snprintf(l2, n2, "graphics -  clock -  (no proj6/7 shared memory at 0x%08x)", VV_SM_BASE_ADDR);
}
uint64_t vv_insns(void) { return insn_count; }
static int reg_index(const char *n) {
    if (n[0] == '$') n++;
    for (int i = 0; i < 32; i++) if (!strcmp(n, gpr[i])) return i;
    char *e; long k = strtol(n, &e, 10); return (*e == 0 && k >= 0 && k < 32) ? (int)k : -1;
}

static void usage(const char *p) {
    fprintf(stderr,
"usage: %s [options] [isr.elf ...] [prog.elf]  (entry = last ELF)\n"
"  --load FILE@ADDR   load raw binary (repeatable); FILE may also be an ELF\n"
"  --entry ADDR       start PC (default: ELF entry, else 0x%08x)\n"
"  --sp ADDR          initial $sp (default 0x50004000)\n"
"  --max-insns N      stop after N instructions (default 1000000, 0=unlimited)\n"
"  --until ADDR       stop when PC reaches ADDR\n"
"  --trace[=N]        print PC of each (or first N) instruction(s)\n"
"  --little / --big   CPU endianness (default %s)\n"
"  --quiet-mmio       don't log MMIO accesses\n"
"  --bios FILE        raw BIOS image loaded at 0x40000000 (default: gios.inst.bin)\n"
"  --no-bios          don't auto-load a BIOS\n"
"  --no-window        don't open the PixelFeeder display window\n"
"  --no-window-keys   don't feed window keystrokes to UART RX (stdin still works)\n"
"  --random-ddr[=SEED] fill DDR with random data at power-up (default on; seed defaults to time)\n"
"  --no-random-ddr    start DDR zeroed instead\n"
"  --uart-in STR      feed STR to UART RX before stdin (\\r \\n escapes; GIOS needs \\r)\n"
"  --dump-frame F.ppm write the selected frame (PF_FRAME) as PPM at exit\n"
"  --trace-regs       --trace plus GPRs changed by the previous insn (debug)\n"
"  --hold             keep the window open after the run until it is closed\n"
"  --dump-lcd         print the 2x16 LCD buffer (text + hex) at exit\n"
"  --screenshot F.png save the whole window (frame + status strip) as PNG at exit\n"
"  --input-script F  timed events (fN|iN key STR / dump F.ppm); enables --irq\n"
"  --irq              inject UART-RX interrupts (EPC/Cause shadowed, ISR at 0x%08x)\n"
"  --dump-every N     dump displayed frame every Nth PF_FRAME write to PREFIX%%06d.ppm\n"
"  --dump-prefix P    prefix for --dump-every (default 'frame')\n"
"  --expect REG=VAL   exit 0 iff GPR REG equals VAL at stop (repeatable)\n"
"  --quirks=LIST      MIPS150 hardware quirks to EMULATE (default 'default' = the current hardware;\n"
"                     env VVIZARD_QUIRKS is applied first). LIST: comma list of default, none, all,\n"
"                     NAME, -NAME, +NAME; a leading -NAME/+NAME edits the set so far. Names:\n"
"                     %s\n"
"  --no-quirks        standard MIPS32 (same as --quirks=none)\n"
"  --list-quirks      print the quirks, their defaults and hardware behaviour, then exit\n"
"  --halt-on-quirk[=LIST] stop (exit 4) with PC, insn, quirk name and registers when the guest hits\n"
"                     a quirk. LIST as for --quirks; default and a leading -NAME/+NAME start from\n"
"                     the emulated set ('all' also reports quirks that are not emulated)\n"
"  --warn-on-quirk[=LIST] same detection, but log each (quirk, pc) once and keep running\n", p, VV_PC_BOOT,
    VV_DEFAULT_BIG_ENDIAN ? "big" : "little", VV_PC_ISR, vv_quirk_names());
}

int main(int argc, char **argv) {
    int big = VV_DEFAULT_BIG_ENDIAN, want_window = 1, hold = 0;
    const char *dump_path = NULL, *bios_path = NULL, *shot_path = NULL; int no_bios = 0, dump_lcd = 0;
    uint32_t entry = VV_PC_BOOT, sp = 0x50004000u; int have_entry = 0, have_elf_entry = 0;
    const char *loads[32]; int nloads = 0;
    const char *expects[32]; int nexp = 0;
    max_insns = 1000000;
    static struct option lo[] = {
        {"load",1,0,'l'},{"entry",1,0,'e'},{"sp",1,0,'s'},{"max-insns",1,0,'m'},{"until",1,0,'u'},
        {"trace",2,0,'t'},{"little",0,0,'L'},{"big",0,0,'B'},{"quiet-mmio",0,0,'q'},{"expect",1,0,'x'},{"input-script",1,0,'S'},{"irq",0,0,'I'},{"dump-every",1,0,'E'},{"dump-prefix",1,0,'P'},
        {"no-window",0,0,'W'},{"no-window-keys",0,0,'K'},{"dump-frame",1,0,'D'},{"hold",0,0,'H'},{"bios",1,0,'b'},{"no-bios",0,0,'N'},{"trace-regs",0,0,'R'},{"random-ddr",2,0,'r'},{"uart-in",1,0,'U'},{"no-random-ddr",0,0,'z'},{"halt-on-quirk",2,0,'Q'},{"warn-on-quirk",2,0,'w'},{"quirks",1,0,'J'},{"no-quirks",0,0,'O'},{"list-quirks",0,0,'Y'},{"dump-lcd",0,0,'C'},{"screenshot",1,0,'G'},{"help",0,0,'h'},{0,0,0,0}};
    int c;
    uint32_t emul = vv_quirk_default();
    const char *qenv = getenv("VVIZARD_QUIRKS"), *qcli = NULL, *qdet = NULL; int qdet_on = 0;
    if (qenv && vv_quirk_parse(qenv, emul, &emul)) { fprintf(stderr, "bad VVIZARD_QUIRKS\n"); return 2; }
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) switch (c) {
        case 'l': if (nloads < 32) loads[nloads++] = optarg; break;
        case 'e': entry = strtoul(optarg, 0, 0); have_entry = 1; break;
        case 's': sp = strtoul(optarg, 0, 0); break;
        case 'm': max_insns = strtoull(optarg, 0, 0); break;
        case 'u': stop_addr = strtoul(optarg, 0, 0); have_stop = 1; break;
        case 't': opt_trace = 1; if (optarg) trace_left = atoi(optarg); break;
        case 'L': big = 0; break;
        case 'B': big = 1; break;
        case 'q': opt_quiet_mmio = 1; break;
        case 'W': want_window = 0; break;
        case 'K': vv_disp_keys_enabled = 0; break;
        case 'D': dump_path = optarg; break;
        case 'H': hold = 1; break;
        case 'b': bios_path = optarg; break;
        case 'N': no_bios = 1; break;
        case 'R': opt_trace = 1; opt_trace_regs = 1; break;
        case 'r': opt_random_ddr = 1; if (optarg) { ddr_seed = strtoull(optarg, NULL, 0); ddr_seed_given = 1; } break;
        case 'z': opt_random_ddr = 0; break;
        case 'U': uart_in = decode_escapes(optarg); break;
        case 'S': if (load_script(optarg)) return 2; opt_irq = 1; break;
        case 'I': opt_irq = 1; break;
        case 'E': dump_every = strtoul(optarg, 0, 0); break;
        case 'P': dump_prefix = optarg; break;
        case 'x': if (nexp < 32) expects[nexp++] = optarg; break;
        case 'Q': case 'w': qdet_on = 1; qdet = optarg; vv_quirk_warn = c == 'w'; break;
        case 'J': qcli = optarg; break;
        case 'O': qcli = "none"; break;
        case 'Y': vv_quirk_list(stdout); return 0;
        case 'C': dump_lcd = 1; break;
        case 'G': shot_path = optarg; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
    }
    if (qcli && vv_quirk_parse(qcli, emul, &emul)) return 2;
    vv_quirk_emul = emul;
    if (qdet_on) {                               /* bare / leading -NAME: relative to the emulated set */
        uint32_t d = emul;
        if (qdet && *qdet && vv_quirk_parse(qdet, emul, &d)) return 2;
        if (qdet && !strcmp(qdet, "default")) d = emul;
        vv_quirk_detect = d;
    }
    uc_engine *uc; uc_err err;
    guest_big = big; vv_lcd_reset();
    if ((err = uc_open(UC_ARCH_MIPS, UC_MODE_MIPS32 | (big ? UC_MODE_BIG_ENDIAN : UC_MODE_LITTLE_ENDIAN), &uc))) {
        fprintf(stderr, "uc_open: %s\n", uc_strerror(err)); return 1; }
    /* Bypass MIPS kseg0/kseg1/kseg2 + TLB: identity-ish translation via our TLB hook. */
    if ((err = uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL))) { fprintf(stderr, "tlb mode: %s\n", uc_strerror(err)); return 1; }
    for (size_t i = 0; i < NREG; i++) {
        const vv_region *r = &regions[i];
        err = r->kind == VV_MMIO ? uc_mmio_map(uc, r->base, r->size, mmio_read, NULL, mmio_write, NULL)
                                 : uc_mem_map(uc, r->base, r->size, UC_PROT_ALL);
        if (err) { fprintf(stderr, "map %s @0x%08x: %s\n", r->name, r->base, uc_strerror(err)); return 1; }
    }
    /* VOID page (quirks unmapped/unmapped-fetch/unaligned/lwlr): reads 0, stores are undone */
    if ((err = uc_mem_map(uc, VV_VOID_PA, 0x1000, UC_PROT_ALL))) { fprintf(stderr, "map VOID: %s\n", uc_strerror(err)); return 1; }
    /* Random DDR before any ELF/--load/BIOS goes in, so images overwrite where they land (as on HW). */
    if (opt_random_ddr) {
        if (!ddr_seed_given) ddr_seed = (uint64_t)time(NULL);
        for (size_t i = 0; i < NREG; i++)
            if (!strcmp(regions[i].name, "DDR")) {
                fprintf(stderr, "[ddr] random fill %u MiB, seed=%llu (repeat with --random-ddr=%llu)\n",
                        regions[i].size / VV_MiB, (unsigned long long)ddr_seed, (unsigned long long)ddr_seed);
                if (fill_ddr_random(uc, regions[i].base, regions[i].size)) return 1;
            }
    }
    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_TLB_FILL, tlb_fill, NULL, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_CODE, hook_code, NULL, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_INTR, hook_intr, NULL, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_MEM_UNMAPPED, hook_unmapped, NULL, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE, hook_bios_write, NULL, 0x40000000ull, 0x4FFFFFFFull);

    for (int i = optind; i < argc; i++) {
        uint32_t e; if (vv_load_elf(uc, argv[i], &e, big)) return 1;
        have_elf_entry = 1; if (!have_entry) entry = e; /* last ELF wins (app after isr) */
    }
    /* BIOS: --bios FILE, else default gios image unless a --load already targets 0x4xxxxxxx. */
    int bios_loaded_by_user = 0;
    for (int i = 0; i < nloads; i++) {
        const char *at = strrchr(loads[i], '@');
        if (at && (strtoul(at + 1, 0, 0) >> 28) == 0x4) bios_loaded_by_user = 1;
    }
    if (!no_bios && (bios_path || !bios_loaded_by_user)) {
        const char *bp = bios_path;
#ifndef VV_DEFAULT_BIOS
        char fb[PATH_MAX + 64];
#endif
        if (!bp) {
#ifdef VV_DEFAULT_BIOS
            bp = VV_DEFAULT_BIOS;
#else
            char exe[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
            exe[n > 0 ? n : 0] = 0;
            snprintf(fb, sizeof fb, "%s/../../software/gios/gios.inst.bin", n > 0 ? dirname(exe) : ".");
            bp = fb;
#endif
        }
        if (access(bp, R_OK) == 0) {
            if (vv_load_raw(uc, bp, VV_PC_BOOT)) return 1;
            fprintf(stderr, "[bios] loaded %s at 0x%08x\n", bp, VV_PC_BOOT);
        } else if (bios_path) { fprintf(stderr, "[bios] cannot read %s\n", bp); return 1; }
        else fprintf(stderr, "[bios] warning: default BIOS %s not found; continuing without BIOS\n", bp);
    }
    for (int i = 0; i < nloads; i++) {
        char *s = strdup(loads[i]), *at = strrchr(s, '@');
        if (vv_is_elf(s) && !at) { uint32_t e; if (vv_load_elf(uc, s, &e, big)) return 1;
            if (!have_entry && !have_elf_entry) { entry = e; have_elf_entry = 1; } free(s); continue; }
        if (!at) { fprintf(stderr, "--load needs FILE@ADDR\n"); return 2; }
        *at = 0; if (vv_load_raw(uc, s, strtoul(at + 1, 0, 0))) return 1; free(s);
    }
    uc_reg_write(uc, UC_MIPS_REG_SP, &sp);
    fprintf(stderr, "[quirk] emulating: %s\n", vv_quirk_set_str(vv_quirk_emul));
    if (vv_quirk_detect) fprintf(stderr, "[quirk] %s on: %s\n", vv_quirk_warn ? "warn" : "halt", vv_quirk_set_str(vv_quirk_detect));
    vv_disp_init(want_window);
    fprintf(stderr, "[run] %s-endian entry=0x%08x sp=0x%08x max-insns=%llu\n", big ? "big" : "little",
            entry, sp, (unsigned long long)max_insns);
    err = uc_emu_start(uc, entry, have_stop ? stop_addr : 0xFFFFFFFFull, 0, max_insns);
    if (err == UC_ERR_EXCEPTION && stop_reason_intr == 18) err = UC_ERR_OK; /* `break` = clean stop */
    if (vv_quirk_halted) err = UC_ERR_OK;    /* reported (and registers dumped) at the quirk */
    /* Unicorn 2.1.1 MIPS reports UC_ERR_EXCEPTION when the uc_emu_start count limit hits
     * (no hook_intr fired); treat that as the normal max-insns stop. */
    if (err == UC_ERR_EXCEPTION && stop_reason_intr < 0 && max_insns && insn_count >= max_insns) err = UC_ERR_OK;
    if (!err && max_insns && insn_count >= max_insns) fprintf(stderr, "[stop] max-insns %llu reached\n", (unsigned long long)max_insns);
    if (err) fprintf(stderr, "[stop] uc_emu_start: %s\n", uc_strerror(err));
    if (opt_irq) fprintf(stderr, "[irq] UARX interrupts injected: %llu, PF_FRAME writes: %llu\n",
                         (unsigned long long)irq_count, (unsigned long long)pf_writes);
    if (vv_quirk_active()) vv_quirk_summary();
    if (!vv_quirk_halted) dump_regs(uc);
    if (dump_path) vv_disp_dump_ppm(uc, dump_path);
    if (dump_lcd) vv_lcd_dump(stderr);
    run_state = vv_quirk_halted ? "HALTED (quirk)" : err ? "STOPPED (error)" : stop_reason_intr == 18 ? "STOPPED (break)" :
                (max_insns && insn_count >= max_insns) ? "STOPPED (max-insns)" : "STOPPED";
    if (shot_path) vv_disp_screenshot(uc, shot_path);
    if (hold) while (!vv_disp_tick(uc)) usleep(16000);
    vv_disp_close();
    int rc = err ? 1 : vv_quirk_halted ? 4 : 0;
    for (int i = 0; i < nexp; i++) {
        char *s = strdup(expects[i]), *eq = strchr(s, '=');
        int r = eq ? (*eq = 0, reg_index(s)) : -1;
        if (r < 0) { fprintf(stderr, "bad --expect %s\n", expects[i]); return 2; }
        uint32_t want = strtoul(eq + 1, 0, 0), got; uc_reg_read(uc, UC_MIPS_REG_0 + r, &got);
        fprintf(stderr, "[expect] $%s = 0x%08x (want 0x%08x) %s\n", s, got, want, got == want ? "PASS" : "FAIL");
        if (got != want) rc = 3;
        free(s);
    }
    uc_close(uc);
    return rc;
}
