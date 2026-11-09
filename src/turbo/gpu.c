/* Geometry adapted from gpu.c / the cs150 command engines. Turbo walks only
 * on this worker: no count pass, ready-edge join, or Unicorn callbacks. */
#include "turbo.h"
#include "display.h"
#include "gpu.h"
#include <errno.h>

#ifdef __APPLE__
static int wake_init(turbo_sem *s) { *s = dispatch_semaphore_create(0); return *s ? 0 : -1; }
static void wake_wait(turbo_sem *s) { dispatch_semaphore_wait(*s, DISPATCH_TIME_FOREVER); }
static void wake_post(turbo_sem *s) { dispatch_semaphore_signal(*s); }
static void wake_destroy(turbo_sem *s) { dispatch_release(*s); }
#else
static int wake_init(turbo_sem *s) { return sem_init(s, 0, 0); }
static void wake_wait(turbo_sem *s) { while (sem_wait(s) && errno == EINTR) {} }
static void wake_post(turbo_sem *s) { sem_post(s); }
static void wake_destroy(turbo_sem *s) { sem_destroy(s); }
#endif
#define W VV_SCREEN_W
#define H VV_SCREEN_H
typedef struct { turbo *t; uint32_t fbase; } gctx;
static void store(gctx *g, uint32_t a, const uint8_t *buf, size_t n) {
    uint8_t *p = turbo_ptr(g->t, a, n); if (!p) return;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v; memcpy(&v, buf + i, 4);
        __atomic_store_n((uint32_t *)(p + i), v, __ATOMIC_RELAXED);
    }
    for (size_t i = 0; i < n; i += 4096) turbo_invalidate(g->t, a + (uint32_t)i);
    if (n) turbo_invalidate(g->t, a + (uint32_t)n - 1);
}
static uint32_t rd_word(turbo *t, uint32_t a, int *ok) {
    uint8_t *p = turbo_ptr(t, a, 4);
    if (!p || (a & 3)) { *ok = 0; return 0; }
    return turbo_word(p);
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
    if (1)
        for (int x = x0; x <= x1; x++) {
            uint8_t *p = buf + 4 * (x - x0);
            p[0] = c >> 24; p[1] = c >> 16; p[2] = c >> 8; p[3] = c;   /* big-endian word */
        }
    store(g, (g->fbase + (uint32_t)y * VV_ROW_PITCH + (uint32_t)x0 * 4), buf, n);
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
    if (1)
        for (uint32_t x = l; x <= r; x++) {
            uint32_t c = (x == l || x == r) ? edge : fill;
            uint8_t *p = buf + 4 * (x - l);
            p[0] = c >> 24; p[1] = c >> 16; p[2] = c >> 8; p[3] = c;
        }
    uint32_t row = g->fbase + y * VV_ROW_PITCH;
    if (dofill || r - l < 2) store(g, (row + l * 4), buf, (size_t)(r - l + 1) * 4);
    else {
        store(g, (row + l * 4), buf, 4);
        store(g, (row + r * 4), buf + 4 * (r - l), 4);
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

static int walk(turbo *t, uint32_t frame, uint32_t code) {
    gctx g = {t, VV_FRAME0 + (turbo_frame(frame) << VV_FSHIFT)};
    int ok = 1;
    for (unsigned n = 0; n < (1u << 20); n++) {
        uint32_t inst = rd_word(t, code, &ok); code += 4;
        if (!ok) return -1;
        unsigned op = inst >> 28; uint32_t color = inst & 0xfffffff;
        if (!inst) return 0;
        if (op == VV_GOP_FILL) {
            for (int y = 0; y < H; y++) slr_run(&g, y, 0, W - 1, color, 0xff000000u | color);
            continue;
        }
        if (op != VV_GOP_LINE && op != VV_GOP_RECT && op != VV_GOP_ELIP) return -1;
        uint32_t a = rd_word(t, code, &ok), b = rd_word(t, code + 4, &ok); code += 8;
        if (!ok) return -1;
        int x0 = (a >> 16) & 1023, y0 = a & 1023, x1 = (b >> 16) & 1023, y1 = b & 1023;
        if (op == VV_GOP_LINE) { do_line(&g, x0, y0, x1, y1, color); continue; }
        uint32_t fill = rd_word(t, code, &ok); code += 4;
        if (!ok) return -1;
        if (op == VV_GOP_RECT) do_rect(&g, x0, y0, x1, y1, color, fill);
        else do_elip(&g, x0, y0, x1, y1, color, fill);
    }
    return -1;
}
static void *worker(void *arg) {
    turbo *t = arg;
    for (;;) {
        wake_wait(&t->gpu_wake);
        if (atomic_load_explicit(&t->gpu_job, memory_order_acquire)) {
            int rc = walk(t, t->job_frame, t->job_code);
            atomic_store_explicit(&t->gpu_fault, rc != 0, memory_order_relaxed);
            atomic_store_explicit(&t->gpu_job, 0, memory_order_release);
        }
        if (atomic_load_explicit(&t->gpu_quit, memory_order_acquire)) break;
    }
    return NULL;
}
int turbo_gpu_start(turbo *t) {
    if (wake_init(&t->gpu_wake)) return -1;
    if (pthread_create(&t->gpu_tid, NULL, worker, t)) { wake_destroy(&t->gpu_wake); return -1; }
    return 0;
}
void turbo_gpu_kick(turbo *t) {
    if (atomic_load_explicit(&t->gpu_job, memory_order_acquire)) { t->drops++; return; }
    t->job_frame = t->gp_frame; t->job_code = t->gp_code;
    t->gp_rframe = turbo_frame(t->gp_frame); t->launches++; t->gp_pending = 1;
    atomic_store_explicit(&t->gpu_fault, 0, memory_order_relaxed);
    atomic_store_explicit(&t->gpu_job, 1, memory_order_release);
    wake_post(&t->gpu_wake);
}
void turbo_gpu_stop(turbo *t) {
    atomic_store_explicit(&t->gpu_quit, 1, memory_order_release);
    wake_post(&t->gpu_wake); pthread_join(t->gpu_tid, NULL); wake_destroy(&t->gpu_wake);
}
