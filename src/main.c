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

static uint64_t mmio_read(uc_engine *uc, uint64_t off, unsigned size, void *u) {
    (void)uc; (void)u; (void)size;
    uint32_t w = (off & 0xFF) & ~3u;            /* addra[5:0] decode: alias every 0x100 */
    uint32_t v = 0;
    switch (w) {
    case VV_OW_UATX_READY: v = 1; break;        /* host stdout always ready */
    case VV_OW_UARX_VALID: if (uart_in && *uart_in) { v = 1; break; }
        { struct pollfd p = { 0, POLLIN, 0 }; v = poll(&p, 1, 0) > 0 && (p.revents & POLLIN); break; }
    case VV_OW_UARX_DATA: if (uart_in && *uart_in) { v = (unsigned char)*uart_in++; break; }
        { unsigned char c = 0; struct pollfd p = { 0, POLLIN, 0 };
        if (poll(&p, 1, 0) > 0 && read(0, &c, 1) == 1) { v = c; } break; }
    case VV_OW_CNT_CYCLE: /* no cycle model: cycles == instructions */
    case VV_OW_CNT_INST:  v = (uint32_t)(insn_count - cnt_base_insn); break;
    case VV_OW_PF_FRAME:  v = 0x10000000u | (((pf_frame >> VV_FSHIFT) & VV_FNMASK) << VV_FSHIFT); break;
    case VV_OW_GP_STATE:  /* graphics.h:72-92 gstate_s: {pf_fault,pf_dormant,pf_frame[5:0], 8'b0,
                           * gp_fault,0,gp_procframe[5:0], 3'b0,rect,elip,line,filler,gp_ready}.
                           * Stub GPU finishes instantly => all engines ready. */
        v = (vv_disp_frame() & 0x3F) << 24
          | (((gp_frame >> 28) ? (gp_frame >> VV_FSHIFT) : gp_frame) & 0x3F) << 16 | 0x1F; break;
    default:
        if (!opt_quiet_mmio) fprintf(stderr, "[mmio] MISS-READ off=0x%03x\n", (unsigned)off);
    }
    /* Sub-word reads (e.g. lbu OB_UARX_DATA = +3 on big-endian): Unicorn hands us the
     * byte offset; return the matching lane of the 32-bit big-endian word. */
    if (size < 4) { unsigned lane = off & 3; v = (v >> (8 * (4 - size - lane))) & ((1u << (8 * size)) - 1); }
    if (!opt_quiet_mmio && w != VV_OW_UATX_READY && w != VV_OW_UARX_VALID)
        fprintf(stderr, "[mmio] R off=0x%03x size=%u -> 0x%08x\n", (unsigned)off, size, v);
    return v;
}
static void mmio_write(uc_engine *uc, uint64_t off, unsigned size, uint64_t val, void *u) {
    (void)u;
    uint32_t w = (off & 0xFF) & ~3u, v = (uint32_t)val;
    switch (w) {
    case VV_OW_UATX_DATA: putchar(v & 0xFF); fflush(stdout); return; /* dina[7:0] */
    case VV_OW_CNT_RESET: cnt_base_insn = insn_count; break;
    case VV_OW_PF_FRAME:  pf_frame = v; vv_disp_pf_write(uc, v); break;
    case VV_OW_GP_FRAME:  gp_frame = v; break;
    case VV_OW_GP_GCODE:  gp_code = v; gp_launches++;   /* write launches GP (MemMapIO.v:50-51) */
        { int n = vv_gpu_run(uc, gp_frame, gp_code, opt_quiet_mmio ? 0 : 1);
          if (!opt_quiet_mmio) fprintf(stderr, "[gpu] launch #%u code@0x%08x frame=0x%08x -> %d cmds\n", gp_launches, gp_code, gp_frame, n); }
        /* HW would raise irq_gp_done (IB_GPU=12) here; interrupts are not injected yet. */
        break;
    default: break;
    }
    if (!opt_quiet_mmio) fprintf(stderr, "[mmio] W off=0x%03x size=%u <- 0x%08x\n", (unsigned)off, size, v);
}

static bool tlb_fill(uc_engine *uc, uint64_t va, uc_mem_type t, uc_tlb_entry *e, void *u) {
    (void)uc; (void)t; (void)u;
    uint32_t pa = vv_canon((uint32_t)va);
    if (pa == 0xFFFFFFFFu) return false;
    e->paddr = pa & ~0xFFFull;                   /* page granularity; offset kept by Unicorn */
    e->perms = region_for((uint32_t)va)->prot;
    return true;
}

static void hook_code(uc_engine *uc, uint64_t addr, uint32_t size, void *u) {
    (void)size; (void)u;
    insn_count++;
    if ((insn_count & 0xFFF) == 0 && vv_disp_tick(uc)) { fprintf(stderr, "[stop] window closed\n"); uc_emu_stop(uc); }
    if (opt_trace && trace_left != 0) {
        uint32_t insn = 0; unsigned char b[4];
        if (!uc_mem_read(uc, vv_canon((uint32_t)addr), b, 4)) insn = b[0]<<24|b[1]<<16|b[2]<<8|b[3];
        fprintf(stderr, "[trace] %8llu pc=0x%08x insn=%08x", (unsigned long long)insn_count, (uint32_t)addr, insn);
        if (opt_trace_regs) { /* GPRs changed by the PREVIOUS instruction (debug aid) */
            for (int i = 1; i < 32; i++) { uint32_t v; uc_reg_read(uc, UC_MIPS_REG_0 + i, &v);
                if (v != prev_regs[i]) { fprintf(stderr, " | r%d=%08x", i, v); prev_regs[i] = v; } }
        }
        fputc('\n', stderr);
        if (trace_left > 0) trace_left--;
    }
}
static int stop_reason_intr = -1;
static void hook_intr(uc_engine *uc, uint32_t intno, void *u) {
    (void)u; uint32_t pc; uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
    stop_reason_intr = intno;
    /* QEMU-MIPS EXCP_* numbering: 8=EXT_INTERRUPT 12=AdEL 13=AdES 14=TLBF 17=SYSCALL 18=BREAK 20=RI
     * (pc reported here is not reliable inside the hook; Unicorn may show 0). */
    const char *nm = intno == 18 ? "BREAK" : intno == 17 ? "SYSCALL" : intno == 8 ? "EXT_INTERRUPT" :
                     intno == 12 ? "AdEL" : intno == 13 ? "AdES" : intno == 20 ? "RI" : "?";
    fprintf(stderr, "[stop] CPU exception intno=%u (%s) pc=0x%08x\n", intno, nm, pc);
    uc_emu_stop(uc);
}
static bool hook_unmapped(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u) {
    (void)uc; (void)u;
    fprintf(stderr, "[fault] unmapped %s at 0x%08x size %d val 0x%llx\n",
            t == UC_MEM_FETCH_UNMAPPED ? "fetch" : t == UC_MEM_WRITE_UNMAPPED ? "write" : "read",
            (uint32_t)a, sz, (long long)v);
    return false;
}
static void hook_bios_write(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u) {
    (void)uc; (void)t; (void)u;
    fprintf(stderr, "[warn] data write to BIOS 0x%08x size %d val 0x%llx (HW drops this)\n", (uint32_t)a, sz, (long long)v);
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
"  --random-ddr[=SEED] fill DDR with random data at power-up (default on; seed defaults to time)\n"
"  --no-random-ddr    start DDR zeroed instead\n"
"  --uart-in STR      feed STR to UART RX before stdin (\\r \\n escapes; GIOS needs \\r)\n"
"  --dump-frame F.ppm write the selected frame (PF_FRAME) as PPM at exit\n"
"  --trace-regs       --trace plus GPRs changed by the previous insn (debug)\n"
"  --hold             keep the window open after the run until it is closed\n"
"  --expect REG=VAL   exit 0 iff GPR REG equals VAL at stop (repeatable)\n", p, VV_PC_BOOT,
    VV_DEFAULT_BIG_ENDIAN ? "big" : "little");
}

int main(int argc, char **argv) {
    int big = VV_DEFAULT_BIG_ENDIAN, want_window = 1, hold = 0;
    const char *dump_path = NULL, *bios_path = NULL; int no_bios = 0;
    uint32_t entry = VV_PC_BOOT, sp = 0x50004000u; int have_entry = 0, have_elf_entry = 0;
    const char *loads[32]; int nloads = 0;
    const char *expects[32]; int nexp = 0;
    max_insns = 1000000;
    static struct option lo[] = {
        {"load",1,0,'l'},{"entry",1,0,'e'},{"sp",1,0,'s'},{"max-insns",1,0,'m'},{"until",1,0,'u'},
        {"trace",2,0,'t'},{"little",0,0,'L'},{"big",0,0,'B'},{"quiet-mmio",0,0,'q'},{"expect",1,0,'x'},
        {"no-window",0,0,'W'},{"dump-frame",1,0,'D'},{"hold",0,0,'H'},{"bios",1,0,'b'},{"no-bios",0,0,'N'},{"trace-regs",0,0,'R'},{"random-ddr",2,0,'r'},{"uart-in",1,0,'U'},{"no-random-ddr",0,0,'z'},{"help",0,0,'h'},{0,0,0,0}};
    int c;
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
        case 'D': dump_path = optarg; break;
        case 'H': hold = 1; break;
        case 'b': bios_path = optarg; break;
        case 'N': no_bios = 1; break;
        case 'R': opt_trace = 1; opt_trace_regs = 1; break;
        case 'r': opt_random_ddr = 1; if (optarg) { ddr_seed = strtoull(optarg, NULL, 0); ddr_seed_given = 1; } break;
        case 'z': opt_random_ddr = 0; break;
        case 'U': uart_in = decode_escapes(optarg); break;
        case 'x': if (nexp < 32) expects[nexp++] = optarg; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
    }
    uc_engine *uc; uc_err err;
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
    vv_disp_init(want_window);
    fprintf(stderr, "[run] %s-endian entry=0x%08x sp=0x%08x max-insns=%llu\n", big ? "big" : "little",
            entry, sp, (unsigned long long)max_insns);
    err = uc_emu_start(uc, entry, have_stop ? stop_addr : 0xFFFFFFFFull, 0, max_insns);
    if (err == UC_ERR_EXCEPTION && stop_reason_intr == 18) err = UC_ERR_OK; /* `break` = clean stop */
    /* Unicorn 2.1.1 MIPS reports UC_ERR_EXCEPTION when the uc_emu_start count limit hits
     * (no hook_intr fired); treat that as the normal max-insns stop. */
    if (err == UC_ERR_EXCEPTION && stop_reason_intr < 0 && max_insns && insn_count >= max_insns) err = UC_ERR_OK;
    if (!err && max_insns && insn_count >= max_insns) fprintf(stderr, "[stop] max-insns %llu reached\n", (unsigned long long)max_insns);
    if (err) fprintf(stderr, "[stop] uc_emu_start: %s\n", uc_strerror(err));
    dump_regs(uc);
    if (dump_path) vv_disp_dump_ppm(uc, dump_path);
    if (hold) while (!vv_disp_tick(uc)) usleep(16000);
    vv_disp_close();
    int rc = err ? 1 : 0;
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
