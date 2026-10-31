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
#include <sys/mman.h>
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#include "term.h"
#include "memmap.h"
#include "loader.h"
#include "display.h"
#include "gpu.h"
#include "quirk.h"
#include "prng.h"
#include "lcd.h"
#include "hw.h"

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
 * base | (addr & (size-1)).  Returns 0xFFFFFFFF for undecoded nibbles.  vv_nib[] is region_for()
 * evaluated once per nibble (init_nib); RAM entries also get their host buffer (main). */
vv_nibble vv_nib[16];
static void init_nib(void) {
    for (unsigned n = 0; n < 16; n++) {
        const vv_region *r = region_for(n << 28);
        vv_nib[n] = r ? (vv_nibble){ NULL, r->base, r->size - 1, 1 } : (vv_nibble){ NULL, 0, 0, 0 };
    }
}
uint32_t vv_canon(uint32_t a) { return vv_canon_inl(a); }

/* ---------------- state ---------------- */
static int opt_trace, opt_quiet_mmio, opt_trace_regs;
static uint32_t prev_regs[32];
static uint64_t insn_count, cnt_base_insn;
static uint64_t max_insns;
static uint32_t stop_addr; static int have_stop;
static uint32_t pf_frame = 0x10000000u, gp_frame, gp_code, gp_launches;
static int trace_left = -1;
/* Guest exit register (VV_OW_GUEST_EXIT, 0x80000084): a store ends the run right after the store
 * instruction; the process exits with code & 0xFF.  --no-guest-exit drops the store (FPGA behaviour). */
static int opt_guest_exit = 1, guest_exited, stop_req;   /* stop_req: hook_code stops before the next insn */
static uint32_t guest_code;

/* ---- UART RX sources: --uart-in, then --input-script bytes that are due, then stdin ---- */
static int stdin_peek = -1, stdin_eof;
static char script_q[4096]; static size_t sq_head, sq_tail;   /* due script bytes */
/* The host end of the serial line (src/hw.c times the bytes onto RX).  poll_stdin: also poll() stdin. */
int vv_host_rx_avail(int poll_stdin) {
    if (uart_in && *uart_in) return 1;
    if (sq_head != sq_tail) return 1;
    if (vv_disp_key_avail()) return 1;
    if (poll_stdin && stdin_peek < 0 && !stdin_eof) {
        struct pollfd p = { 0, POLLIN, 0 }; unsigned char c;
        if (poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) {
            if (read(0, &c, 1) == 1) stdin_peek = c; else stdin_eof = 1; }
    }
    return stdin_peek >= 0;
}
/* --uart-in is typed by a patient host: its next byte waits until the guest has read the last one
 * (then the --uart-rx-gap idle time), so auto-typing a command never overruns RX.  Script keys,
 * window keys and stdin are not paced: they arrive when due, as a person types. */
int vv_host_rx_paced(void) { return uart_in && *uart_in; }
int vv_host_rx_take(void) {
    if (!vv_host_rx_avail(1)) return 0;
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
static int opt_irq = 1;                  /* take COP0150 interrupts (default; --no-irq turns off) */
static unsigned dump_every; static const char *dump_prefix = "frame";
static void run_event(uc_engine *uc, vv_event *e) {
    if (e->is_dump) vv_disp_dump_ppm(uc, e->arg);
    else for (char *c = e->arg; *c; c++) {
        size_t n = (sq_tail + 1) % sizeof script_q;
        if (n == sq_head) { vv_eprintf("[script] RX queue full\n"); break; }
        script_q[sq_tail] = *c; sq_tail = n; vv_disp_key_log((unsigned char)*c);
    }
    vv_eprintf("[script] %c%llu %s %s (insn %llu, frame %llu)\n", e->is_frame ? 'f' : 'i',
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
            vv_eprintf("%s:%d: bad line\n", path, ln); fclose(f); return -1; }
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

/* ---- Interrupt entry (COP0150 + CPUMIPS.v DO_ISR) ----
 * The Cause latch, Count/Compare and the device interrupt sources are in src/hw.c.  Unicorn 2.1.1
 * can't set Cause IP bits or expose Cause/EPC (NOTES.md), and its exception entry (MIPS vector, EXL)
 * differs from COP0150, so the entry is done here: when Status.IE and any Status.IM & Cause.IP bit
 * are set (and the deferral rules of CPUMIPS.v:114 allow it, below), EPC = pc, Status.IE = 0 and the PC
 * jumps to PC_ISR (0xC0000180); the insn at pc runs after the ISR returns.  With the cop0
 * quirk on (default) mfc0/mtc0 use the hw.c registers (quirk.c); with it off, Status is Unicorn's
 * and mfc0/mtc0 $13/$14 inside the ISR BRAM are patched to the hw.c Cause/EPC.  The ISR returns
 * with `jr $k0; mtc0 Status` (eret is a nop on the MIPS150).  Leaving the ISR (IE 0 -> 1) with the
 * entry's Cause bit still pending and enabled is the irq-storm guest bug. */
static int fix_rt = -1; static uint32_t fix_val;
#define NOPC 0xFFFFFFFFu
static uint32_t last_pc = NOPC, last2_pc = NOPC, last3_pc = NOPC;   /* the last three insns run */
static uint64_t irq_count;
static uint32_t prev_status;
/* The word at a as the hardware fetches it: 0 for undecoded / MMIO (and no MMIO side effects). */
static uint32_t read_insn(uc_engine *uc, uint32_t a) { (void)uc; int ok; return vv_ram_be32(vv_canon_inl(a), &ok); }
static int is_mtc0(uint32_t i) { return (i >> 21) == 0x204; }   /* 010000 00100 (CPUMIPS.v:113) */
static int is_branch(uint32_t i) {
    unsigned op = i >> 26, fn = i & 0x3F;
    return (op == 0 && (fn == 8 || fn == 9)) || op == 1 || (op >= 2 && op <= 7) || (op >= 20 && op <= 23);
}
void vv_mmio_side_read(uint32_t canon) {         /* a HW read of UARX_DATA pops the RX holding register */
    if ((canon & VV_MMIO_DECODE) == VV_OW_UARX_DATA) (void)vv_uart_rx_read();
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
                fix_rt = rt; fix_val = rd == 14 ? vv_hw_epc() : vv_hw_cause();
            } else if (rs == 4 && (rd == 13 || rd == 14)) { /* mtc0 */
                uc_reg_read(uc, UC_MIPS_REG_0 + rt, &v);
                if (rd == 13) vv_cop0_write(13, v);       /* COP0150: IP bits 15:10 only clear */
                else vv_hw_set_epc(v);
            }
        }
    }
    /* Fast path: no Cause IP bit latched and not inside an ISR.  Then no interrupt can be requested
     * (vv_hw_irq_request needs Status & Cause & IP) and the IE-edge check below has nothing to do
     * (vv_hw_isr_exit_check returns at once outside an ISR; prev_status is set again at ISR entry
     * before it matters), so Status need not be read. */
    if (!vv_hw_irq_possible()) {
        if (vv_quirk_halted) return;
        last3_pc = last2_pc; last2_pc = last_pc; last_pc = pc;
        return;
    }
    uint32_t st;
    if (cop0) st = vv_hw_status(); else uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &st);
    if ((st & 1) && !(prev_status & 1)) vv_hw_isr_exit_check(st, pc, "IE re-enabled");   /* irq-storm */
    prev_status = st;
    if (vv_quirk_halted) return;
    /* CPUMIPS.v:114 DO_ISR replaces the fetch AFTER the instruction in F (X, the one that just ran here),
     * so EPC = pc.  Not when X could branch, X is an mtc0, or X is a taken branch's target (WAS_Branch:
     * the branch two instructions back was taken); WAS_ISR needs IE, which entry cleared. */
    if (vv_hw_irq_request(st)) {
        int defer = 0;
        if (last_pc != NOPC) { uint32_t x = read_insn(uc, last_pc); defer = is_branch(x) || is_mtc0(x); }
        if (!defer && last3_pc != NOPC && is_branch(read_insn(uc, last3_pc)) && last_pc != last3_pc + 8) defer = 1;
        if (!defer) {
            vv_hw_isr_enter(pc, st); st &= ~1u; prev_status = st;
            if (!cop0) uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &st);
            uint32_t npc = VV_PC_ISR; irq_count++;
            last_pc = last2_pc = last3_pc = NOPC;
            uc_reg_write(uc, UC_MIPS_REG_PC, &npc);   /* insn at pc not executed yet; resumes at ISR */
            return;
        }
    }
    last3_pc = last2_pc; last2_pc = last_pc; last_pc = pc;
}

static uint64_t mmio_read(uc_engine *uc, uint64_t off, unsigned size, void *u) {
    (void)uc; (void)u; (void)size;
    uint32_t w = off & VV_MMIO_DECODE;          /* addra[11:0]; registers only at addra[11:6]==0 */
    uint32_t v = 0;
    if (w >= VV_MMIO_REG_SPAN) {                /* LCD window reads 0; elsewhere HW reads 32'dx */
        if (!opt_quiet_mmio && (w < VV_OW_LCD_BASE || w >= VV_OW_LCD_END))
            vv_eprintf("[mmio] MISS-READ off=0x%04x\n", (unsigned)off);
        w = 0xFFFFFFFFu;                        /* no register: v stays 0 */
    }
    switch (w) {
    case VV_OW_UATX_READY: v = vv_uart_tx_ready(); break;   /* low while a byte shifts out (hw.c) */
    case VV_OW_UARX_VALID: v = vv_uart_rx_valid(); break;
    case VV_OW_UARX_DATA:  v = vv_uart_rx_read(); break;
    case VV_OW_CNT_CYCLE: /* no cycle model: cycles == instructions */
    case VV_OW_CNT_INST:  v = (uint32_t)(insn_count - cnt_base_insn); break;
    case VV_OW_PF_FRAME:  v = 0x10000000u | vv_pf_frame() << VV_FSHIFT; break;   /* frame being scanned (MemMapIO.v:162) */
    case VV_OW_GP_STATE:  v = vv_gp_status_word(); break;   /* graphics.h gstate_s layout (hw.c) */
    case VV_OW_RANDOM32:  v = vv_prng_read(insn_count); break;  /* cycle == insn count (see prng.c) */
    case 0xFFFFFFFFu: break;
    default:
        if (!opt_quiet_mmio) vv_eprintf("[mmio] MISS-READ off=0x%04x\n", (unsigned)off);
    }
    /* Sub-word reads (e.g. lbu OB_UARX_DATA = +3 on big-endian): Unicorn hands us the
     * byte offset; return the matching lane of the 32-bit big-endian word. */
    if (size < 4) { unsigned lane = off & 3; v = (v >> (8 * (4 - size - lane))) & ((1u << (8 * size)) - 1); }
    if (!opt_quiet_mmio && w != VV_OW_UATX_READY && w != VV_OW_UARX_VALID)
        vv_eprintf("[mmio] R off=0x%04x size=%u -> 0x%08x\n", (unsigned)off, size, v);
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
            if (!opt_quiet_mmio) vv_eprintf("[mmio] W off=0x%04x size=%u <- 0x%08x (LCD[%u] = 0x%02x)\n",
                                         (unsigned)off, size, v, (unsigned)(w - VV_OW_LCD_BASE) >> 2, v & 0xFF);
        } else if (!opt_quiet_mmio) vv_eprintf("[mmio] MISS-WRITE off=0x%04x size=%u <- 0x%08x (dropped)\n", (unsigned)off, size, v);
        return;
    }
    switch (w) {
    case VV_OW_UATX_DATA: vv_uart_tx_write(v & 0xFF); return;    /* dina[7:0]; lost while TX busy */
    case VV_OW_CNT_RESET: cnt_base_insn = insn_count; break;
    case VV_OW_PF_FRAME:  pf_frame = v; vv_pf_write(uc, v); pf_writes++;   /* takes effect at the frame end (hw.c) */
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
    case VV_OW_GUEST_EXIT:   /* sb/sh: the lane-placed dina word (as RANDOM32), e.g. sb at +3 = the byte */
        if (size < 4 && !lane_hw) { unsigned lane = off & 3;
            v = (v & ((1u << (8 * size)) - 1)) << (8 * (4 - size - lane)); }
        if (!opt_guest_exit) {
            static int noted;
            if (!noted++) vv_eprintf("[exit] guest exit code %u (0x%08x) ignored (--no-guest-exit)\n", v, v);
            break;
        }
        if (!guest_exited) {
            guest_exited = 1; guest_code = v; stop_req = 1;
            vv_eprintf("[exit] guest exit code %d (0x%08x) at pc=0x%08x -> exit status %u\n", (int32_t)v, v, vv_cur_pc, v & 0xFF);
            /* hook_code stops before the next insn runs */
        }
        break;
    case VV_OW_GP_GCODE:  gp_code = v; gp_launches++;   /* write launches GP (MemMapIO.v:50-51) */
        { int n = vv_gpu_run(uc, gp_frame, gp_code, opt_quiet_mmio ? 0 : 1);
          vv_gp_launched(gp_frame, n);                 /* GP status frame/fault + irq_gp_done (Cause bit 12) */
          if (!opt_quiet_mmio) vv_eprintf("[gpu] launch #%u code@0x%08x frame=0x%08x -> %d cmds\n", gp_launches, gp_code, gp_frame, n); }
        break;
    default: break;
    }
    if (!opt_quiet_mmio) vv_eprintf("[mmio] W off=0x%04x size=%u <- 0x%08x\n", (unsigned)off, size, v);
}

static int stop_reason_intr = -1;
static bool tlb_fill(uc_engine *uc, uint64_t va, uc_mem_type t, uc_tlb_entry *e, void *u) {
    (void)uc; (void)t; (void)u;
    uint32_t pa = vv_canon((uint32_t)va);
    int fetch = t == UC_MEM_FETCH, hole = pa == 0xFFFFFFFFu;
    if (fetch && (hole || (pa >> 28) == 0x8)) {   /* HW fetches DEAD_IMEM = 0 (MemBank.v:132) */
        if ((vv_quirk_active() >> VV_Q_UNMAPPED_FETCH & 1) && stop_reason_intr < 0 && /* not Unicorn's own exception vector */
            !(max_insns && insn_count >= max_insns) && !stop_req)   /* nor its fetch on the way out of a stop */
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

/* --max-insns: counts code-hook calls exactly as uc_emu_start's count limit did (Unicorn's own
 * count hook ran first on every instruction, so the N+1st call stops before anything else runs),
 * without making Unicorn add a second per-instruction hook. */
static uint64_t hook_calls;
static int stopped_max;
static void hook_code(uc_engine *uc, uint64_t addr, uint32_t size, void *u) {
    (void)size; (void)u;
    if (stop_req) { uc_emu_stop(uc); return; }   /* guest exit: nothing after the store runs */
    if (max_insns && ++hook_calls > max_insns) { stopped_max = 1; uc_emu_stop(uc); return; }
    insn_count++;
    vv_cur_pc = (uint32_t)addr;
    vv_hw_tick(uc, insn_count);              /* devices advance one cycle (UART, PixelFeeder, timer) */
    if (vv_quirk_halted) { uc_emu_stop(uc); return; }
    if (nevents) run_due_events(uc, 0);
    if (!vv_quirk_decode) {                  /* no quirk needs the per-instruction decode (quirk.h) */
        uint64_t irqs = irq_count;
        if (opt_irq) irq_step(uc, (uint32_t)addr);
        if (vv_quirk_active()) {             /* device-side detectors only: eret irq-storm check */
            if (irqs == irq_count && !vv_quirk_halted) vv_quirk_pre_lite(uc, (uint32_t)addr);
            if (vv_quirk_halted) return;
        }
    } else {                                 /* emulated quirks and/or decode-based detectors */
        if (vv_quirk_halted) { uc_emu_stop(uc); return; }
        if (vv_quirk_post(uc, (uint32_t)addr)) { insn_count--; return; }   /* fix up the previous insn; emulated branch */
        uint64_t irqs = irq_count;
        if (opt_irq) irq_step(uc, (uint32_t)addr);
        if (irqs != irq_count) vv_quirk_cancel();   /* redirected to the ISR: insn at addr not run */
        else if (!vv_quirk_halted) vv_quirk_pre(uc, (uint32_t)addr);
        if (vv_quirk_halted) return;
    }
    if ((insn_count & 0xFFF) == 0) {
        vv_term_tick();                      /* flush pending UART text (e.g. a prompt with no newline) */
        int why = vv_disp_tick(uc);
        if (why) { vv_eprintf(why == VV_DISP_ESC ? "[stop] Esc pressed\n" : "[stop] window closed\n"); uc_emu_stop(uc); }
    }
    if (opt_trace && trace_left != 0) {
        uint32_t insn = read_insn(uc, (uint32_t)addr);
        vv_eprintf("[trace] %8llu pc=0x%08x insn=%08x", (unsigned long long)insn_count, (uint32_t)addr, insn);
        if (opt_trace_regs) { /* GPRs changed by the PREVIOUS instruction (debug aid) */
            for (int i = 1; i < 32; i++) { uint32_t v; uc_reg_read(uc, UC_MIPS_REG_0 + i, &v);
                if (v != prev_regs[i]) { char n[8], b[32]; snprintf(n, sizeof n, "r%d", i);
                    vv_eprintf(" | %s=%08x", vv_creg(n, b, sizeof b), v); prev_regs[i] = v; } }
        }
        vv_eprintf("\n");
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
    vv_eprintf("[stop] CPU exception intno=%u (%s) pc=0x%08x\n", intno, nm, pc);
    uc_emu_stop(uc);
}
static bool hook_unmapped(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u) {
    (void)uc; (void)u;
    if (vv_quirk_halted) return false;
    vv_eprintf("[fault] unmapped %s at 0x%08x size %d val 0x%llx\n",
            t == UC_MEM_FETCH_UNMAPPED ? "fetch" : t == UC_MEM_WRITE_UNMAPPED ? "write" : "read",
            (uint32_t)a, sz, (long long)v);
    return false;
}
static void hook_bios_write(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u) {
    (void)uc; (void)t; (void)u;
    vv_eprintf("[warn] data write to BIOS 0x%08x size %d val 0x%llx (HW drops this; %s)\n", (uint32_t)a, sz, (long long)v,
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

/* Same bytes as the old 1 MiB uc_mem_write chunks of host-order uint64s, generated straight into
 * the host buffer (nothing has been translated yet, so no code cache to invalidate). */
static int fill_ddr_random(uint32_t base, uint32_t size) {
    uint8_t *p = vv_ram(base, size);
    if (!p || size % 8) { vv_eprintf("random-ddr @0x%08x: not a host-mapped RAM region\n", base); return 1; }
    uint64_t st = ddr_seed;
    for (uint32_t off = 0; off < size; off += 8) { uint64_t v = splitmix64(&st); memcpy(p + off, &v, 8); }
    return 0;
}

static const char *gpr[32] = {"zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
static void dump_regs(uc_engine *uc) {
    uint32_t v; char b[32];
    vv_eprintf("---- registers (insns=%llu) ----\n", (unsigned long long)insn_count);
    for (int i = 0; i < 32; i++) {                /* "%4s=" with the name highlighted (term.c) */
        uc_reg_read(uc, UC_MIPS_REG_0 + i, &v);
        vv_eprintf("%*s%s=%08x%s", 4 - (int)strlen(gpr[i]), "", vv_creg(gpr[i], b, sizeof b), v, (i % 4 == 3) ? "\n" : "  ");
    }
    uc_reg_read(uc, UC_MIPS_REG_PC, &v); vv_eprintf("  %s=%08x  ", vv_creg("pc", b, sizeof b), v);
    uc_reg_read(uc, UC_MIPS_REG_HI, &v); vv_eprintf("%s=%08x  ", vv_creg("hi", b, sizeof b), v);
    uc_reg_read(uc, UC_MIPS_REG_LO, &v); vv_eprintf("%s=%08x  ", vv_creg("lo", b, sizeof b), v);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &v); vv_eprintf("%s=%08x  ", vv_creg("c0_status", b, sizeof b), v);
    vv_eprintf("\n"); /* Unicorn 2.1 exposes no Cause/EPC/Count reg ids */
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

/* ---- built-in defaults: `./vvizard` with no arguments = the proj7 player via GIOS ----
 * Software tree: --sw-dir DIR, else <directory of the executable>/../../software (the cs150 layout
 * <project>/extern/vvizard/vvizard + <project>/software), so it works from any cwd. */
#define VV_SW_REL     "../../software"
#define VV_BIOS_REL   "gios/gios.inst.bin"
#define VV_AUTORUN    "jal 60000000\\r"       /* typed at the GIOS prompt (GIOS ends tokens on CR) */
static const struct { const char *rel; uint32_t addr; } default_app[] = {
    { "draw/draw.inst.bin",       0x50000000u },   /* D-scratch */
    { "proj_app7/isr7.bin",       0xC0000000u },   /* ISR BRAM */
    { "proj_app7/proj7.inst.bin", 0x60000000u },   /* I-scratch: the app GIOS jumps to */
};
#define NDEFAULT_APP (sizeof default_app / sizeof default_app[0])
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
/* Directory holding the running executable (no trailing slash); "." if unknown. */
static void exe_dir(const char *argv0, char *out, size_t n) {
    char raw[PATH_MAX] = "", real[PATH_MAX];
#if defined(__APPLE__)
    uint32_t sz = sizeof raw; if (_NSGetExecutablePath(raw, &sz) != 0) raw[0] = 0;
#elif defined(__linux__)
    ssize_t k = readlink("/proc/self/exe", raw, sizeof raw - 1); raw[k > 0 ? k : 0] = 0;
#endif
    if (!raw[0] && argv0 && strchr(argv0, '/')) snprintf(raw, sizeof raw, "%s", argv0);
    if (raw[0] && realpath(raw, real)) snprintf(out, n, "%s", dirname(real));
    else snprintf(out, n, ".");
}
static char sw_default[PATH_MAX + 32];   /* <exe dir>/../../software, resolved when it exists */
static void init_sw_default(const char *argv0) {
    char d[PATH_MAX], p[PATH_MAX + 32];
    exe_dir(argv0, d, sizeof d);
    snprintf(p, sizeof p, "%s/" VV_SW_REL, d);
    if (!realpath(p, sw_default)) snprintf(sw_default, sizeof sw_default, "%s", p);
}

static void usage(FILE *f, const char *p) {   /* --help: stdout; a bad option: stderr */
    fprintf(f,
"usage: %s [options] [isr.elf ...] [prog.elf]  (entry = last ELF)\n"
"Every option takes --opt=VALUE or --opt VALUE.  With no arguments vvizard runs the proj7 player:\n"
"\n"
"Default program (cs150 software tree SW = --sw-dir, default <exe dir>/" VV_SW_REL ",\n"
"  here %s):\n"
"  BIOS SW/" VV_BIOS_REL " @0x%08x, SW/%s @0x%08x, SW/%s @0x%08x,\n"
"  SW/%s @0x%08x, entry 0x%08x, autorun '" VV_AUTORUN "' typed at the GIOS prompt.\n"
"  Giving any --load FILE@ADDR or ELF file replaces the default program (no default loads, no\n"
"  autorun); the BIOS is still loaded unless a --load targets 0x4xxxxxxx or --no-bios is given.\n"
"  --bare             run only the BIOS (no default loads, no autorun): the GIOS prompt\n"
"  --sw-dir DIR       software tree for the default program and BIOS\n"
"  --bios FILE        BIOS image loaded at 0x%08x instead of SW/" VV_BIOS_REL "\n"
"  --no-bios          don't load a BIOS\n"
"  --uart-in STR      feed STR to UART RX before stdin, replacing the autorun (\\r \\n escapes)\n"
"  --no-autorun       don't type the default autorun command\n"
"\n"
"Program:\n"
"  --load FILE@ADDR   load raw binary (repeatable); FILE may also be an ELF\n"
"  --entry ADDR       start PC (default: ELF entry, else 0x%08x)\n"
"  --sp ADDR          preset $sp (default: none; registers start at 0 as on hardware, and reading\n"
"                     one that was never written is the undef-reg guest bug)\n"
"  --little / --big   CPU endianness (default %s)\n"
"\n"
"Run control (defaults: unlimited, interrupts on, random DDR, window with keys):\n"
"  --max-insns N      stop after N instructions (default 0 = unlimited)\n"
"  --until ADDR       stop when PC reaches ADDR\n"
"  --irq / --no-irq   take COP0150 interrupts (default on; ISR at 0x%08x)\n"
"  --random-ddr[=SEED] fill DDR with random data at power-up (default on; seed defaults to time)\n"
"  --no-random-ddr    start DDR zeroed instead\n"
"  --window / --no-window  PixelFeeder display window (default on)\n"
"  --no-window-keys   don't feed window keystrokes to UART RX (stdin still works)\n"
"  --hold             keep the window open after the run until it is closed\n"
"  --input-script F   timed events (fN|iN key STR / dump F.ppm)\n"
"  --expect REG=VAL   exit 0 iff GPR REG equals VAL at stop (repeatable)\n"
"  --no-guest-exit    ignore stores to the guest exit register 0x%08x (FPGA behaviour: the\n"
"                     guest goes on, e.g. into its interrupts-off spin loop)\n"
"  Esc in the window ends the run like closing it (Esc is not sent to the UART)\n"
"\n"
"Exit status: 0 normal stop (break, --max-insns, --until, window closed, Esc), 1 setup or CPU\n"
"  error, 2 usage, 3 an --expect failed, 4 --halt-on-quirk; a guest exit-register store: the\n"
"  stored code & 0xFF (so guest codes 1-4 look like vvizard's own; the [exit] line tells them\n"
"  apart), unless an --expect fails (3)\n"
"\n"
"Hardware model:\n"
"  --legacy-io        old untimed I/O: TX always ready, RX served on demand with a level UARX interrupt,\n"
"                     PF_FRAME switches at once, no TX/GP/PF/timer/RTC interrupts\n"
"  --uart-rx-gap N    idle cycles the host leaves between UART RX bytes (default %u = one byte time;\n"
"                     0 = back-to-back at 115200 baud)\n"
"  --pf-period N      cycles per PixelFeeder frame (default %u = 1040x666 @ 40 MHz in 50 MHz cycles;\n"
"                     0 = PF_FRAME writes take effect at once, no frame interrupt)\n"
"\n"
"Quirks (LIST: comma list edited left to right: NAME or +NAME adds, -NAME or no-NAME removes,\n"
"  none / all / default replace the set so far; e.g. --quirks=+jalr-rd,no-load-use, --quirks=none,hilo):\n"
"  --quirks=LIST      MIPS150 hardware quirks to EMULATE, starting from 'default' (= the current\n"
"                     hardware; env VVIZARD_QUIRKS is applied first; repeatable, each edits the set). Names:\n"
"                     %s\n"
"  --no-quirks        standard MIPS32 (same as --quirks=none)\n"
"  --list-quirks      print the quirks, their defaults and hardware behaviour, then exit\n"
"  --halt-on-quirk[=LIST] stop (exit 4) with PC, insn, quirk name and registers when the guest hits\n"
"                     a quirk.  DEFAULT ON for the emulated set.  LIST starts from the emulated set\n"
"                     ('all' also reports quirks that are not emulated)\n"
"  --warn-on-quirk[=LIST] same detection, but log each (quirk, pc) once and keep running\n"
"  --no-halt-on-quirk don't detect emulated quirks (the guest-bug detectors dcache-stale, tx-busy,\n"
"                     rx-overrun, irq-storm, undef-reg still warn unless silenced)\n"
"  --quiet-quirk=LIST don't report these categories at all\n"
"\n"
"Output:\n"
"  --quiet-mmio       don't log MMIO accesses (default)\n"
"  --mmio-log         log every MMIO access and GP launch (alias --no-quiet-mmio)\n"
"  --trace[=N]        print PC of each (or first N) instruction(s)\n"
"  --trace-regs       --trace plus GPRs changed by the previous insn (debug)\n"
"  --dump-frame F.ppm write the displayed frame as PPM at exit\n"
"  --dump-every N     dump displayed frame every Nth PF_FRAME write to PREFIX%%06d.ppm\n"
"  --dump-prefix P    prefix for --dump-every (default 'frame')\n"
"  --dump-lcd         print the 2x16 LCD buffer (text + hex) at exit\n"
"  --screenshot F.png save the whole window (frame + status strip) as PNG at exit\n"
"  --color[=WHEN]     WHEN: auto (default), always (= bare --color) or never.  stdout carries only\n"
"                     the guest's UART output and stderr everything else; with color, UART text is\n"
"                     bold green and stderr [tags] / register names are highlighted (stderr only\n"
"                     when it is a terminal, or with always).  auto: on iff stdout is a terminal\n"
"                     and NO_COLOR is unset (and TERM is not dumb)\n"
"  -h, --help         this text\n", p, sw_default, VV_PC_BOOT,
    default_app[0].rel, default_app[0].addr, default_app[1].rel, default_app[1].addr,
    default_app[2].rel, default_app[2].addr, VV_PC_BOOT, VV_PC_BOOT, VV_PC_BOOT,
    VV_DEFAULT_BIG_ENDIAN ? "big" : "little", VV_PC_ISR, VV_MMIO_BASE + VV_OW_GUEST_EXIT, VV_UART_RX_FRAME, VV_PF_PERIOD, vv_quirk_names());
}

/* getopt_long's optional arguments only bind with '=' (--color=never).  To accept the
 * `--color never` form as well, take the next word when it is a valid value for the option
 * (so `--trace prog.elf` still treats prog.elf as a program). */
static int is_uint(const char *s) { char *e; if (!*s || *s == '-') return 0; strtoull(s, &e, 0); return !*e; }
static int is_color(const char *s) { return vv_color_parse(s) >= 0; }
static const char *opt_next(int argc, char **argv, int (*ok)(const char *)) {
    if (optarg) return optarg;
    if (optind < argc && argv[optind][0] != '-' && ok(argv[optind])) return argv[optind++];
    return NULL;
}

enum { O_LOAD = 256, O_ENTRY, O_SP, O_MAX, O_UNTIL, O_TRACE, O_LITTLE, O_BIG, O_QUIET_MMIO, O_MMIO_LOG,
       O_EXPECT, O_SCRIPT, O_IRQ, O_NO_IRQ, O_DUMP_EVERY, O_DUMP_PREFIX, O_WINDOW, O_NO_WINDOW,
       O_NO_KEYS, O_DUMP_FRAME, O_HOLD, O_BIOS, O_NO_BIOS, O_TRACE_REGS, O_RANDOM_DDR, O_NO_RANDOM_DDR,
       O_UART_IN, O_NO_AUTORUN, O_HALT, O_WARN, O_NO_HALT, O_QUIRKS, O_NO_QUIRKS, O_LIST_QUIRKS,
       O_DUMP_LCD, O_SCREENSHOT, O_LEGACY_IO, O_RX_GAP, O_PF_PERIOD, O_QUIET_QUIRK, O_COLOR,
       O_BARE, O_SW_DIR, O_GUEST_EXIT, O_NO_GUEST_EXIT };

int main(int argc, char **argv) {
    int big = VV_DEFAULT_BIG_ENDIAN, want_window = 1, hold = 0;
    const char *dump_path = NULL, *bios_path = NULL, *shot_path = NULL; int no_bios = 0, dump_lcd = 0;
    uint32_t entry = VV_PC_BOOT, sp = 0; int have_entry = 0, have_elf_entry = 0, have_sp = 0;
    const char *loads[32]; int nloads = 0;
    const char *expects[32]; int nexp = 0;
    int bare = 0, uart_in_set = 0; const char *sw_dir = NULL;
    max_insns = 0;                               /* unlimited */
    opt_quiet_mmio = 1;
    init_sw_default(argv[0]);
    static struct option lo[] = {
        {"load",1,0,O_LOAD},{"entry",1,0,O_ENTRY},{"sp",1,0,O_SP},{"max-insns",1,0,O_MAX},{"until",1,0,O_UNTIL},
        {"trace",2,0,O_TRACE},{"little",0,0,O_LITTLE},{"big",0,0,O_BIG},{"quiet-mmio",0,0,O_QUIET_MMIO},
        {"mmio-log",0,0,O_MMIO_LOG},{"no-quiet-mmio",0,0,O_MMIO_LOG},{"expect",1,0,O_EXPECT},
        {"input-script",1,0,O_SCRIPT},{"irq",0,0,O_IRQ},{"no-irq",0,0,O_NO_IRQ},{"dump-every",1,0,O_DUMP_EVERY},
        {"dump-prefix",1,0,O_DUMP_PREFIX},{"window",0,0,O_WINDOW},{"no-window",0,0,O_NO_WINDOW},
        {"no-window-keys",0,0,O_NO_KEYS},{"dump-frame",1,0,O_DUMP_FRAME},{"hold",0,0,O_HOLD},
        {"bios",1,0,O_BIOS},{"no-bios",0,0,O_NO_BIOS},{"trace-regs",0,0,O_TRACE_REGS},
        {"random-ddr",2,0,O_RANDOM_DDR},{"no-random-ddr",0,0,O_NO_RANDOM_DDR},{"uart-in",1,0,O_UART_IN},
        {"no-autorun",0,0,O_NO_AUTORUN},{"halt-on-quirk",2,0,O_HALT},{"warn-on-quirk",2,0,O_WARN},
        {"no-halt-on-quirk",0,0,O_NO_HALT},{"quirks",1,0,O_QUIRKS},{"no-quirks",0,0,O_NO_QUIRKS},
        {"list-quirks",0,0,O_LIST_QUIRKS},{"dump-lcd",0,0,O_DUMP_LCD},{"screenshot",1,0,O_SCREENSHOT},
        {"help",0,0,'h'},{"legacy-io",0,0,O_LEGACY_IO},{"uart-rx-gap",1,0,O_RX_GAP},{"pf-period",1,0,O_PF_PERIOD},
        {"quiet-quirk",1,0,O_QUIET_QUIRK},{"color",2,0,O_COLOR},{"bare",0,0,O_BARE},{"sw-dir",1,0,O_SW_DIR},
        {"guest-exit",0,0,O_GUEST_EXIT},{"no-guest-exit",0,0,O_NO_GUEST_EXIT},
        {0,0,0,0}};
    int c, color = VV_COLOR_AUTO;
    uint32_t emul = vv_quirk_default(), qdflt = vv_quirk_default();
    const char *qenv = getenv("VVIZARD_QUIRKS"), *qdet = NULL, *qquiet = NULL;
    int qdet_on = 1, qwarn = 0;                  /* default: --halt-on-quirk (emulated set) */
    if (qenv && vv_quirk_parse(qenv, emul, qdflt, &emul)) { vv_eprintf("bad VVIZARD_QUIRKS\n"); return 2; }
    while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) { const char *v; switch (c) {
        case O_LOAD: if (nloads < 32) loads[nloads++] = optarg; break;
        case O_ENTRY: entry = strtoul(optarg, 0, 0); have_entry = 1; break;
        case O_SP: sp = strtoul(optarg, 0, 0); have_sp = 1; break;
        case O_MAX: max_insns = strtoull(optarg, 0, 0); break;
        case O_UNTIL: stop_addr = strtoul(optarg, 0, 0); have_stop = 1; break;
        case O_TRACE: opt_trace = 1; trace_left = -1; if ((v = opt_next(argc, argv, is_uint))) trace_left = atoi(v); break;
        case O_LITTLE: big = 0; break;
        case O_BIG: big = 1; break;
        case O_QUIET_MMIO: opt_quiet_mmio = 1; break;
        case O_MMIO_LOG: opt_quiet_mmio = 0; break;
        case O_WINDOW: want_window = 1; break;
        case O_NO_WINDOW: want_window = 0; break;
        case O_NO_KEYS: vv_disp_keys_enabled = 0; break;
        case O_DUMP_FRAME: dump_path = optarg; break;
        case O_HOLD: hold = 1; break;
        case O_BIOS: bios_path = optarg; no_bios = 0; break;
        case O_NO_BIOS: no_bios = 1; bios_path = NULL; break;
        case O_TRACE_REGS: opt_trace = 1; opt_trace_regs = 1; break;
        case O_RANDOM_DDR: opt_random_ddr = 1;
            if ((v = opt_next(argc, argv, is_uint))) { ddr_seed = strtoull(v, NULL, 0); ddr_seed_given = 1; } break;
        case O_NO_RANDOM_DDR: opt_random_ddr = 0; break;
        case O_UART_IN: uart_in = decode_escapes(optarg); uart_in_set = 1; break;
        case O_NO_AUTORUN: uart_in = NULL; uart_in_set = 1; break;
        case O_SCRIPT: if (load_script(optarg)) return 2; break;
        case O_IRQ: opt_irq = 1; break;
        case O_NO_IRQ: opt_irq = 0; break;
        case O_DUMP_EVERY: dump_every = strtoul(optarg, 0, 0); break;
        case O_DUMP_PREFIX: dump_prefix = optarg; break;
        case O_EXPECT: if (nexp < 32) expects[nexp++] = optarg; break;
        case O_HALT: case O_WARN: qdet_on = 1; qdet = opt_next(argc, argv, vv_quirk_list_ok); qwarn = c == O_WARN; break;
        case O_NO_HALT: qdet_on = 0; qdet = NULL; break;
        case O_QUIET_QUIRK: qquiet = optarg; break;
        case O_LEGACY_IO: vv_legacy_io = 1; break;
        case O_RX_GAP: vv_uart_rx_gap = strtoull(optarg, 0, 0); break;
        case O_PF_PERIOD: vv_pf_period = strtoull(optarg, 0, 0); break;
        case O_QUIRKS: if (vv_quirk_parse(optarg, emul, qdflt, &emul)) return 2; break;
        case O_NO_QUIRKS: emul = 0; break;
        case O_LIST_QUIRKS: vv_quirk_list(stdout); return 0;
        case O_DUMP_LCD: dump_lcd = 1; break;
        case O_SCREENSHOT: shot_path = optarg; break;
        case O_COLOR: v = opt_next(argc, argv, is_color);
            if ((color = vv_color_parse(v)) < 0) {
                vv_eprintf("--color: expected always, never or auto, not '%s'\n", v); return 2; } break;
        case O_BARE: bare = 1; break;
        case O_SW_DIR: sw_dir = optarg; break;
        case O_GUEST_EXIT: opt_guest_exit = 1; break;
        case O_NO_GUEST_EXIT: opt_guest_exit = 0; break;
        case 'h': usage(stdout, argv[0]); return 0;
        default: usage(stderr, argv[0]); return 2;
    } }
    /* The default program applies unless --bare, or the command line names a program itself. */
    int user_prog = nloads > 0 || optind < argc;
    int use_default_app = !bare && !user_prog;
    const char *sw = sw_dir ? sw_dir : sw_default;
    char app_paths[NDEFAULT_APP][2 * PATH_MAX];
    if (use_default_app) {
        for (size_t i = 0; i < NDEFAULT_APP; i++) {
            snprintf(app_paths[i], sizeof app_paths[i], "%s/%s@0x%08X", sw, default_app[i].rel, default_app[i].addr);
            char f[2 * PATH_MAX]; snprintf(f, sizeof f, "%s/%s", sw, default_app[i].rel);
            if (access(f, R_OK)) {
                vv_eprintf("default program: cannot read %s\n  (software tree %s; use --sw-dir DIR, or --bare / "
                           "--load FILE@ADDR to run something else; --help)\n", f, sw);
                return 1;
            }
            loads[nloads++] = app_paths[i];
        }
        if (!uart_in_set) uart_in = decode_escapes(VV_AUTORUN);
    }
    vv_term_init(color);
    vv_quirk_emul = emul;
    uint32_t det = 0, quiet = 0;
    if (qdet_on) {                               /* bare / leading -NAME: relative to the emulated set */
        if (vv_quirk_parse(qdet, emul, emul, &det)) return 2;   /* 'default' here = the emulated set */
    }
    if (qquiet && vv_quirk_parse(qquiet, 0, qdflt, &quiet)) return 2;
    vv_quirk_halt = qdet_on && !qwarn ? det & ~quiet : 0;
    vv_quirk_detect = (det | vv_quirk_bugs()) & ~quiet;   /* guest-bug detectors warn by default */
    uc_engine *uc; uc_err err;
    guest_big = big; vv_lcd_reset();
    if ((err = uc_open(UC_ARCH_MIPS, UC_MODE_MIPS32 | (big ? UC_MODE_BIG_ENDIAN : UC_MODE_LITTLE_ENDIAN), &uc))) {
        vv_eprintf("uc_open: %s\n", uc_strerror(err)); return 1; }
    /* Bypass MIPS kseg0/kseg1/kseg2 + TLB: identity-ish translation via our TLB hook. */
    if ((err = uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL))) { vv_eprintf("tlb mode: %s\n", uc_strerror(err)); return 1; }
    /* RAM regions are host buffers (uc_mem_map_ptr) so the hooks read guest memory directly
     * (vv_ram, memmap.h).  mmap: page aligned and zeroed like uc_mem_map's own RAM (macOS: MAP_ANON). */
    init_nib();
    void *ram_buf[NREG] = { 0 };
    for (size_t i = 0; i < NREG; i++) {
        const vv_region *r = &regions[i];
        if (r->kind == VV_MMIO) err = uc_mmio_map(uc, r->base, r->size, mmio_read, NULL, mmio_write, NULL);
        else {
            void *m = mmap(NULL, r->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (m == MAP_FAILED) { vv_eprintf("map %s: cannot allocate %u bytes\n", r->name, r->size); return 1; }
            ram_buf[i] = m;
            err = uc_mem_map_ptr(uc, r->base, r->size, UC_PROT_ALL, m);
            for (unsigned n = 0; !err && n < 16; n++) if (vv_nib[n].ok && vv_nib[n].base == r->base) vv_nib[n].host = m;
        }
        if (err) { vv_eprintf("map %s @0x%08x: %s\n", r->name, r->base, uc_strerror(err)); return 1; }
    }
    /* VOID page (quirks unmapped/unmapped-fetch/unaligned/lwlr): reads 0, stores are undone */
    if ((err = uc_mem_map(uc, VV_VOID_PA, 0x1000, UC_PROT_ALL))) { vv_eprintf("map VOID: %s\n", uc_strerror(err)); return 1; }
    /* Random DDR before any ELF/--load/BIOS goes in, so images overwrite where they land (as on HW). */
    if (opt_random_ddr) {
        if (!ddr_seed_given) ddr_seed = (uint64_t)time(NULL);
        for (size_t i = 0; i < NREG; i++)
            if (!strcmp(regions[i].name, "DDR")) {
                vv_eprintf("[ddr] random fill %u MiB, seed=%llu (repeat with --random-ddr=%llu)\n",
                        regions[i].size / VV_MiB, (unsigned long long)ddr_seed, (unsigned long long)ddr_seed);
                if (fill_ddr_random(regions[i].base, regions[i].size)) return 1;
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
    if (use_default_app) vv_eprintf("[app] default program from %s (--bare, --load or an ELF replaces it; --help)\n", sw);
    /* BIOS: --bios FILE, else SW/gios image unless a --load already targets 0x4xxxxxxx. */
    int bios_loaded_by_user = 0;
    for (int i = 0; i < nloads; i++) {
        const char *at = strrchr(loads[i], '@');
        if (at && (strtoul(at + 1, 0, 0) >> 28) == 0x4) bios_loaded_by_user = 1;
    }
    if (!no_bios && (bios_path || !bios_loaded_by_user)) {
        char fb[2 * PATH_MAX];
        const char *bp = bios_path;
        if (!bp) { snprintf(fb, sizeof fb, "%s/" VV_BIOS_REL, sw); bp = fb; }
        if (access(bp, R_OK) == 0) {
            if (vv_load_raw(uc, bp, VV_PC_BOOT)) return 1;
            vv_eprintf("[bios] loaded %s at 0x%08x\n", bp, VV_PC_BOOT);
        } else if (bios_path || use_default_app || bare) {
            vv_eprintf("[bios] cannot read %s%s\n", bp, bios_path ? "" : " (use --sw-dir DIR, --bios FILE or --no-bios)");
            return 1;
        } else vv_eprintf("[bios] warning: default BIOS %s not found; continuing without BIOS\n", bp);
    }
    for (int i = 0; i < nloads; i++) {
        char *s = strdup(loads[i]), *at = strrchr(s, '@');
        if (vv_is_elf(s) && !at) { uint32_t e; if (vv_load_elf(uc, s, &e, big)) return 1;
            if (!have_entry && !have_elf_entry) { entry = e; have_elf_entry = 1; } free(s); continue; }
        if (!at) { vv_eprintf("--load needs FILE@ADDR\n"); return 2; }
        *at = 0; if (vv_load_raw(uc, s, strtoul(at + 1, 0, 0))) return 1; free(s);
    }
    if (have_sp) { uc_reg_write(uc, UC_MIPS_REG_SP, &sp); vv_quirk_reg_defined(29); }
    vv_quirk_init(uc);
    vv_eprintf("[quirk] emulating: %s\n", vv_quirk_set_str(vv_quirk_emul));
    if (vv_quirk_halt) vv_eprintf("[quirk] halt on: %s\n", vv_quirk_set_str(vv_quirk_halt));
    if (vv_quirk_detect & ~vv_quirk_halt) vv_eprintf("[quirk] warn on: %s\n", vv_quirk_set_str(vv_quirk_detect & ~vv_quirk_halt));
    vv_disp_init(want_window);
    vv_eprintf("[run] %s-endian entry=0x%08x sp=%s max-insns=%llu io=%s\n", big ? "big" : "little",
            entry, have_sp ? "preset" : "0 (not preset)", (unsigned long long)max_insns, vv_legacy_io ? "legacy" : "timed");
    vv_quirk_decode = vv_quirk_needs_decode();
    err = uc_emu_start(uc, entry, have_stop ? stop_addr : 0xFFFFFFFFull, 0, 0);   /* max_insns: hook_code */
    if (err == UC_ERR_EXCEPTION && stop_reason_intr == 18) err = UC_ERR_OK; /* `break` = clean stop */
    if (vv_quirk_halted) err = UC_ERR_OK;    /* reported (and registers dumped) at the quirk */
    /* (Unicorn 2.1.1 MIPS reported UC_ERR_EXCEPTION when uc_emu_start's own count limit hit, with no
     * hook_intr; hook_code's uc_emu_stop returns OK, but keep treating that as the max-insns stop.) */
    if (err == UC_ERR_EXCEPTION && stop_reason_intr < 0 && max_insns && insn_count >= max_insns) err = UC_ERR_OK;
    int at_max = max_insns && (stopped_max || insn_count >= max_insns);
    if (!err && at_max) vv_eprintf("[stop] max-insns %llu reached\n", (unsigned long long)max_insns);
    if (err) vv_eprintf("[stop] uc_emu_start: %s\n", uc_strerror(err));
    if (opt_irq) vv_eprintf("[irq] interrupts taken: %llu, PF_FRAME writes: %llu\n",
                         (unsigned long long)irq_count, (unsigned long long)pf_writes);
    vv_hw_summary();
    if (vv_quirk_active()) vv_quirk_summary();
    if (!vv_quirk_halted) dump_regs(uc);
    if (dump_path) vv_disp_dump_ppm(uc, dump_path);
    if (dump_lcd) vv_lcd_dump();
    static char exit_state[48];
    if (guest_exited) snprintf(exit_state, sizeof exit_state, "EXITED (guest code %d)", (int32_t)guest_code);
    run_state = vv_quirk_halted ? "HALTED (quirk)" : err ? "STOPPED (error)" : guest_exited ? exit_state : stop_reason_intr == 18 ? "STOPPED (break)" :
                at_max ? "STOPPED (max-insns)" : "STOPPED";
    vv_disp_status_dirty();
    if (shot_path) vv_disp_screenshot(uc, shot_path);
    if (hold) while (!vv_disp_tick(uc)) usleep(16000);
    vv_disp_close();
    int rc = err ? 1 : vv_quirk_halted ? 4 : guest_exited ? (int)(guest_code & 0xFF) : 0;
    for (int i = 0; i < nexp; i++) {
        char *s = strdup(expects[i]), *eq = strchr(s, '=');
        int r = eq ? (*eq = 0, reg_index(s)) : -1;
        if (r < 0) { vv_eprintf("bad --expect %s\n", expects[i]); return 2; }
        uint32_t want = strtoul(eq + 1, 0, 0), got; uc_reg_read(uc, UC_MIPS_REG_0 + r, &got);
        vv_eprintf("[expect] $%s = 0x%08x (want 0x%08x) %s\n", s, got, want, got == want ? "PASS" : "FAIL");
        if (got != want) rc = 3;
        free(s);
    }
    uc_close(uc);
    for (size_t i = 0; i < NREG; i++) if (ram_buf[i]) munmap(ram_buf[i], regions[i].size);
    return rc;
}
