/* gpu.c - GraphicsProcessor command interpreter (instant completion).
 * Evidence: hardware/src/gpu/gpcommands.vh, GraphicsProcessor.v, RectangleEngine.v,
 * ScanLineRunner.v, FrameFiller.v; software/common/graphics.h. */
#include "term.h"
#include "gpu.h"
#include "memmap.h"
#include "loader.h"
#include "display.h"
#include "dcache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

#define W VV_SCREEN_W
#define H VV_SCREEN_H
#define MAX_WORDS (1u << 20)        /* runaway guard: no STOP within 4 MiB of words */

static vv_gpu_stats st;

const vv_gpu_stats *vv_gpu_get_stats(void) { return &st; }

/* One walk of a command list.  G_UC: today's --sync path (D-cache DMA marks + uc_mem_write, on the
 * emulator).  G_COUNT: no stores, only the pixel count (emulator, at a threaded launch).  G_HOST:
 * the GPU worker thread: stores straight into the DDR host buffer, no uc_*, no D-cache, no printing. */
enum { G_UC, G_COUNT, G_HOST };
typedef struct { uc_engine *uc; uint32_t fbase; int mode, quiet, nonram; uint64_t pix; } gctx;

static uint32_t rd_word(gctx *g, uint32_t a, int *ok) {
    uint8_t b[4]; uint32_t c = vv_canon(a); int in_ram = 1;
    uint32_t v = vv_ram_be32(c, &in_ram);                  /* RAM: straight from the host buffer */
    if (in_ram) return v;
    g->nonram = 1;
    if (g->mode == G_HOST) { *ok = 0; return 0; }          /* (worker: never uc_*; the count pass
                                                            * on the emulator saw the same list) */
    if (uc_mem_read(g->uc, c, b, 4)) { *ok = 0; return 0; }   /* (MMIO etc.: as before) */
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}
static void store(gctx *g, uint32_t a, const uint8_t *buf, size_t n) {
    g->pix += n / 4;
    if (g->mode == G_COUNT) return;
    if (g->mode == G_HOST) { uint8_t *p = vv_ram(a, (uint32_t)n); if (p) memcpy(p, buf, n); return; }
    vv_dc_dma_write(g->uc, a, n);               /* DDR write behind the CPU D-cache (dcache-stale) */
    uc_mem_write(g->uc, a, buf, n);
}
/* Write a horizontal span [x0,x1] on row y, clipped to the 800x600 screen. */
static void span(gctx *g, int y, int x0, int x1, uint32_t c) {
    uint8_t buf[W * 4];
    if (y < 0 || y >= H) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (x0 > x1) return;
    size_t n = (size_t)(x1 - x0 + 1) * 4;
    if (g->mode != G_COUNT)
        for (int x = x0; x <= x1; x++) {
            uint8_t *p = buf + 4 * (x - x0);
            p[0] = c >> 24; p[1] = c >> 16; p[2] = c >> 8; p[3] = c;   /* big-endian word */
        }
    store(g, vv_canon(g->fbase + (uint32_t)y * VV_ROW_PITCH + (uint32_t)x0 * 4), buf, n);
}
static void pixel(gctx *g, int x, int y, uint32_t c) { span(g, y, x, x, c); }

/* Row with edge pixels at both ends and optional interior fill (ScanLineRunner.v:127-147:
 * interior written only if fill color[31:24] != 0). */
static void edge_row(gctx *g, int y, int l, int r, uint32_t edge, uint32_t fill) {
    if (l > r) { int t = l; l = r; r = t; }
    pixel(g, l, y, edge);
    pixel(g, r, y, edge);
    if ((fill & 0xFF000000u) && r - l > 1) span(g, y, l + 1, r - 1, fill);
}

static void do_line(gctx *g, int x0, int y0, int x1, int y1, uint32_t c) {
    /* Integer Bresenham, both endpoints inclusive. */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x1 >= x0 ? 1 : -1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1, sy = y1 >= y0 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        pixel(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}
static void do_rect(gctx *g, int xa, int ya, int xb, int yb, uint32_t edge, uint32_t fill) {
    int t = ya < yb ? ya : yb, b = ya < yb ? yb : ya;   /* RectangleEngine.v:106 orders y */
    for (int y = t; y <= b; y++) {
        if (y == t || y == b) span(g, y, xa, xb, edge);  /* RectangleEngine.v:127 top/bottom solid */
        else edge_row(g, y, xa, xb, edge, fill);
    }
}
/* ScanLineRunner-equivalent run writer for ellipse spans: 10-bit wrapped row/cols, start/finish
 * swapped if reversed (ScanLineRunner.v:84), edge colour at both ends, fill strictly between only
 * if fill[31:24] != 0 (ScanLineRunner.v:118-147). No clipping: off-screen rows/cols (<1024) land in
 * frame DRAM just like hardware (1024 rows x 4096 B = 4 MiB frame region). */
static void slr_run(gctx *g, uint32_t y, uint32_t l, uint32_t r, uint32_t edge, uint32_t fill) {
    uint8_t buf[1024 * 4];
    y &= 1023; l &= 1023; r &= 1023;
    if (l > r) { uint32_t t = l; l = r; r = t; }
    int dofill = (fill & 0xFF000000u) != 0;
    if (g->mode != G_COUNT)
        for (uint32_t x = l; x <= r; x++) {
            uint32_t c = (x == l || x == r) ? edge : fill;
            uint8_t *p = buf + 4 * (x - l);
            p[0] = c >> 24; p[1] = c >> 16; p[2] = c >> 8; p[3] = c;
        }
    uint32_t row = g->fbase + y * VV_ROW_PITCH;
    if (dofill || r - l < 2) store(g, vv_canon(row + l * 4), buf, (size_t)(r - l + 1) * 4);
    else {
        store(g, vv_canon(row + l * 4), buf, 4);
        store(g, vv_canon(row + r * 4), buf + 4 * (r - l), 4);
    }
}
static void elip_rows(gctx *g, uint32_t xc, uint32_t yc, uint32_t x, uint32_t y, uint32_t e, uint32_t f) {
    slr_run(g, yc - y, xc - x, xc + x, e, f);   /* SLa1/SLb1: yT */
    slr_run(g, yc + y, xc - x, xc + x, e, f);   /* SLa2/SLb2: yB */
}
/* Direct port of ElipseEngine.v midpoint algorithm (reference C at ElipseEngine.v:316-357),
 * 32-bit arithmetic as in the RTL, loop guard ElipseEngine.v:259. x,y are 10-bit registers. */
static void do_elip(gctx *g, uint32_t xc, uint32_t yc, uint32_t a, uint32_t b, uint32_t edge, uint32_t fill) {
    uint32_t x = 0, y = b;
    uint32_t AA = a * a, BB = b * b, AABB = AA * BB;
    uint32_t AAy = AA * b, BBx = 0, BB2xp3 = (BB << 1) + BB;
    int32_t stopper = (int32_t)((AA >> 1) + BB);
    int32_t dd = (int32_t)(BB + (AA >> 2) - AAy);
    for (;;) {                                   /* Phase A */
        elip_rows(g, xc, yc, x, y, edge, fill);
        int32_t dd_next = dd; uint32_t y_next = y, AAy_next = AAy;
        if (dd >= 0) { dd_next = (int32_t)((uint32_t)dd + (AA << 1) - (AAy << 1)); y_next = (y - 1) & 1023; AAy_next = AAy - AA; }
        if (!(y > 0 && x <= a && (AAy - BBx) > (uint32_t)stopper)) break;
        dd = (int32_t)((uint32_t)dd_next + BB2xp3);
        y = y_next; AAy = AAy_next; x = (x + 1) & 1023; BBx += BB; BB2xp3 += BB << 1;
    }
    uint32_t BB2xp2 = BB2xp3 - BB;               /* Phase B */
    uint32_t ym1 = (y - 1) & 1023;
    dd = (int32_t)(BB * (x * x + x) + (BB >> 2) + AA * (ym1 * ym1) - AABB);
    while (y > 0) {
        uint32_t add = 0;
        if (dd < 0) { add = BB2xp2; x = (x + 1) & 1023; BB2xp2 += BB << 1; }
        dd = (int32_t)((uint32_t)dd + add + (AA << 1) + AA - (AAy << 1));
        y--; AAy -= AA;
        elip_rows(g, xc, yc, x, y, edge, fill);
    }
}

/* Walk the list at code (at most max_cmds commands).  G_HOST prints nothing and keeps no stats.
 * *ncmds: commands walked (including STOP / the faulting one). */
static int walk(gctx *g, uint32_t gp_frame, uint32_t code, int verbose, uint32_t max_cmds, int *ncmds) {
    int quiet = g->quiet;
    uint32_t fn = (gp_frame >> 28) ? (gp_frame >> VV_FSHIFT) & VV_FNMASK : gp_frame & VV_FNMASK; /* FRAME_BITS */
    g->fbase = VV_FRAME0 + (fn << VV_FSHIFT);
    if (!quiet) { st.launches++; st.last_frame = fn; }
    int n = 0, ok = 1;
    uint32_t pc = code;
    for (uint32_t i = 0; i < MAX_WORDS && (uint32_t)n < max_cmds; i++) {
        uint32_t inst = rd_word(g, pc, &ok); pc += 4;
        if (!ok) break;
        unsigned op = inst >> 28;
        uint32_t color = inst & 0x0FFFFFFFu;  /* GraphicsProcessor.v:154 {4'b0, COLOR28} */
        n++;
        if (op == VV_GOP_STOP) { if (!quiet) st.cmds += n; *ncmds = n; return n; }
        if (op == VV_GOP_FILL) {               /* FrameFiller: whole 800x600 */
            for (int y = 0; y < H; y++) span(g, y, 0, W - 1, color);
            continue;
        }
        if (op != VV_GOP_LINE && op != VV_GOP_ELIP && op != VV_GOP_RECT) {
            if (!quiet) { vv_eprintf("[gpu] fault: bad opcode %u (0x%08x) at 0x%08x\n", op, inst, pc - 4);
                          st.faults++; st.cmds += n; }
            *ncmds = n;
            return -2;                         /* GP_fault (GraphicsProcessor.v:188-189) */
        }
        uint32_t p0 = rd_word(g, pc, &ok), p1 = rd_word(g, pc + 4, &ok); pc += 8;
        /* gpcommands.vh:23-26 X=25:16, Y=9:0 (XVAR/YVAR flag fields ignored) */
        int x0 = (p0 >> 16) & 0x3FF, y0 = p0 & 0x3FF, x1 = (p1 >> 16) & 0x3FF, y1 = p1 & 0x3FF;
        if (op == VV_GOP_LINE) { do_line(g, x0, y0, x1, y1, color); continue; }
        uint32_t fill = rd_word(g, pc, &ok); pc += 4;
        if (!ok) break;
        if (op == VV_GOP_RECT) do_rect(g, x0, y0, x1, y1, color, fill);
        else                   do_elip(g, x0, y0, x1, y1, color, fill);
        if (verbose > 1 && !quiet) vv_eprintf("[gpu] op%u (%d,%d)-(%d,%d) c=%07x f=%08x\n", op, x0, y0, x1, y1, color, fill);
    }
    *ncmds = n;
    if (quiet) return -1;
    st.faults++; st.cmds += n;
    vv_eprintf("[gpu] fault: command list at 0x%08x ended without STOP (%d cmds)\n", code, n);
    return -1;
}

int vv_gpu_run(uc_engine *uc, uint32_t gp_frame, uint32_t code, int verbose) {
    gctx g = { uc, 0, G_UC, 0, 0, 0 }; int nc;
    return walk(&g, gp_frame, code, verbose, UINT32_MAX, &nc);
}
int vv_gpu_count(uc_engine *uc, uint32_t gp_frame, uint32_t code, int verbose, uint64_t *pixels, int *ncmds, int *host_only) {
    gctx g = { uc, 0, G_COUNT, 0, 0, 0 };
    int r = walk(&g, gp_frame, code, verbose, UINT32_MAX, ncmds);
    *pixels = g.pix; *host_only = !g.nonram; return r;
}
void vv_gpu_draw_uc(uc_engine *uc, uint32_t gp_frame, uint32_t code, int ncmds) {
    gctx g = { uc, 0, G_UC, 1, 0, 0 }; int nc;   /* the stores of vv_gpu_run, no stats / messages */
    walk(&g, gp_frame, code, 0, (uint32_t)ncmds, &nc);
}

/* ---- GPU worker thread (docs/MULTITHREADING-PLAN.md section 4.3): one job slot ---- */
static pthread_t wk_tid; static int wk_started, wk_quit, wk_pending;
static pthread_mutex_t wk_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wk_job = PTHREAD_COND_INITIALIZER, wk_fin = PTHREAD_COND_INITIALIZER;
static atomic_int wk_done = 1;
static uint32_t job_frame, job_code, job_fn; static int job_cmds;   /* job_fn: emulator only */
static void *worker(void *a) {
    (void)a;
    pthread_mutex_lock(&wk_mx);
    for (;;) {
        while (!wk_pending && !wk_quit) pthread_cond_wait(&wk_job, &wk_mx);
        if (wk_quit) break;
        wk_pending = 0;
        uint32_t f = job_frame, c = job_code; int n = job_cmds;
        pthread_mutex_unlock(&wk_mx);
        gctx g = { NULL, 0, G_HOST, 1, 0, 0 }; int nc;
        walk(&g, f, c, 0, (uint32_t)n, &nc);
        pthread_mutex_lock(&wk_mx);
        atomic_store_explicit(&wk_done, 1, memory_order_release);
        pthread_cond_broadcast(&wk_fin);
    }
    pthread_mutex_unlock(&wk_mx);
    return NULL;
}
int vv_gpu_worker_start(void) {
    if (wk_started) return 0;
    if (pthread_create(&wk_tid, NULL, worker, NULL)) return -1;
    wk_started = 1; return 0;
}
void vv_gpu_submit(uint32_t gp_frame, uint32_t code, int ncmds) {
    pthread_mutex_lock(&wk_mx);
    job_frame = gp_frame; job_code = code; job_cmds = ncmds; wk_pending = 1;
    job_fn = (gp_frame >> 28) ? (gp_frame >> VV_FSHIFT) & VV_FNMASK : gp_frame & VV_FNMASK;
    atomic_store_explicit(&wk_done, 0, memory_order_relaxed);
    pthread_cond_signal(&wk_job);
    pthread_mutex_unlock(&wk_mx);
}
int vv_gpu_done(void) { return atomic_load_explicit(&wk_done, memory_order_acquire); }
void vv_gpu_wait(void) {
    for (int i = 0; i < 2000; i++) if (vv_gpu_done()) return;   /* usually already done */
    pthread_mutex_lock(&wk_mx);
    while (!atomic_load_explicit(&wk_done, memory_order_acquire)) pthread_cond_wait(&wk_fin, &wk_mx);
    pthread_mutex_unlock(&wk_mx);
}
void vv_gpu_settle(uint32_t fn) {
    if (wk_started && !vv_gpu_done() && job_fn == fn) vv_gpu_wait();
}
void vv_gpu_worker_stop(void) {
    if (!wk_started) return;
    vv_gpu_wait();
    pthread_mutex_lock(&wk_mx); wk_quit = 1; pthread_cond_signal(&wk_job); pthread_mutex_unlock(&wk_mx);
    pthread_join(wk_tid, NULL); wk_started = 0;
}
