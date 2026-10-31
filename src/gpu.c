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

#define W VV_SCREEN_W
#define H VV_SCREEN_H
#define MAX_WORDS (1u << 20)        /* runaway guard: no STOP within 4 MiB of words */

static vv_gpu_stats st;
static uint32_t fbase;

const vv_gpu_stats *vv_gpu_get_stats(void) { return &st; }

static uint32_t rd_word(uc_engine *uc, uint32_t a, int *ok) {
    uint8_t b[4];
    if (uc_mem_read(uc, vv_canon(a), b, 4)) { *ok = 0; return 0; }
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}
/* Write a horizontal span [x0,x1] on row y, clipped to the 800x600 screen. */
static void span(uc_engine *uc, int y, int x0, int x1, uint32_t c) {
    static uint8_t buf[W * 4];
    if (y < 0 || y >= H) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (x0 > x1) return;
    for (int x = x0; x <= x1; x++) {
        uint8_t *p = buf + 4 * (x - x0);
        p[0] = c >> 24; p[1] = c >> 16; p[2] = c >> 8; p[3] = c;   /* big-endian word */
    }
    uint32_t a = vv_canon(fbase + (uint32_t)y * VV_ROW_PITCH + (uint32_t)x0 * 4); size_t n = (size_t)(x1 - x0 + 1) * 4;
    vv_dc_dma_write(uc, a, n);                  /* DDR write behind the CPU D-cache (dcache-stale) */
    uc_mem_write(uc, a, buf, n);
}
static void pixel(uc_engine *uc, int x, int y, uint32_t c) { span(uc, y, x, x, c); }

/* Row with edge pixels at both ends and optional interior fill (ScanLineRunner.v:127-147:
 * interior written only if fill color[31:24] != 0). */
static void edge_row(uc_engine *uc, int y, int l, int r, uint32_t edge, uint32_t fill) {
    if (l > r) { int t = l; l = r; r = t; }
    pixel(uc, l, y, edge);
    pixel(uc, r, y, edge);
    if ((fill & 0xFF000000u) && r - l > 1) span(uc, y, l + 1, r - 1, fill);
}

static void do_line(uc_engine *uc, int x0, int y0, int x1, int y1, uint32_t c) {
    /* Integer Bresenham, both endpoints inclusive. */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x1 >= x0 ? 1 : -1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1, sy = y1 >= y0 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        pixel(uc, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}
static void do_rect(uc_engine *uc, int xa, int ya, int xb, int yb, uint32_t edge, uint32_t fill) {
    int t = ya < yb ? ya : yb, b = ya < yb ? yb : ya;   /* RectangleEngine.v:106 orders y */
    for (int y = t; y <= b; y++) {
        if (y == t || y == b) span(uc, y, xa, xb, edge);  /* RectangleEngine.v:127 top/bottom solid */
        else edge_row(uc, y, xa, xb, edge, fill);
    }
}
/* ScanLineRunner-equivalent run writer for ellipse spans: 10-bit wrapped row/cols, start/finish
 * swapped if reversed (ScanLineRunner.v:84), edge colour at both ends, fill strictly between only
 * if fill[31:24] != 0 (ScanLineRunner.v:118-147). No clipping: off-screen rows/cols (<1024) land in
 * frame DRAM just like hardware (1024 rows x 4096 B = 4 MiB frame region). */
static void slr_run(uc_engine *uc, uint32_t y, uint32_t l, uint32_t r, uint32_t edge, uint32_t fill) {
    static uint8_t buf[1024 * 4];
    y &= 1023; l &= 1023; r &= 1023;
    if (l > r) { uint32_t t = l; l = r; r = t; }
    int dofill = (fill & 0xFF000000u) != 0;
    for (uint32_t x = l; x <= r; x++) {
        uint32_t c = (x == l || x == r) ? edge : fill;
        uint8_t *p = buf + 4 * (x - l);
        p[0] = c >> 24; p[1] = c >> 16; p[2] = c >> 8; p[3] = c;
    }
    uint32_t row = fbase + y * VV_ROW_PITCH;
    if (dofill || r - l < 2) {
        vv_dc_dma_write(uc, vv_canon(row + l * 4), (size_t)(r - l + 1) * 4);
        uc_mem_write(uc, vv_canon(row + l * 4), buf, (size_t)(r - l + 1) * 4);
    } else {
        vv_dc_dma_write(uc, vv_canon(row + l * 4), 4);
        uc_mem_write(uc, vv_canon(row + l * 4), buf, 4);
        vv_dc_dma_write(uc, vv_canon(row + r * 4), 4);
        uc_mem_write(uc, vv_canon(row + r * 4), buf + 4 * (r - l), 4);
    }
}
static void elip_rows(uc_engine *uc, uint32_t xc, uint32_t yc, uint32_t x, uint32_t y, uint32_t e, uint32_t f) {
    slr_run(uc, yc - y, xc - x, xc + x, e, f);   /* SLa1/SLb1: yT */
    slr_run(uc, yc + y, xc - x, xc + x, e, f);   /* SLa2/SLb2: yB */
}
/* Direct port of ElipseEngine.v midpoint algorithm (reference C at ElipseEngine.v:316-357),
 * 32-bit arithmetic as in the RTL, loop guard ElipseEngine.v:259. x,y are 10-bit registers. */
static void do_elip(uc_engine *uc, uint32_t xc, uint32_t yc, uint32_t a, uint32_t b, uint32_t edge, uint32_t fill) {
    uint32_t x = 0, y = b;
    uint32_t AA = a * a, BB = b * b, AABB = AA * BB;
    uint32_t AAy = AA * b, BBx = 0, BB2xp3 = (BB << 1) + BB;
    int32_t stopper = (int32_t)((AA >> 1) + BB);
    int32_t dd = (int32_t)(BB + (AA >> 2) - AAy);
    for (;;) {                                   /* Phase A */
        elip_rows(uc, xc, yc, x, y, edge, fill);
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
        elip_rows(uc, xc, yc, x, y, edge, fill);
    }
}

int vv_gpu_run(uc_engine *uc, uint32_t gp_frame, uint32_t code, int verbose) {
    uint32_t fn = (gp_frame >> 28) ? (gp_frame >> VV_FSHIFT) & VV_FNMASK : gp_frame & VV_FNMASK; /* FRAME_BITS */
    fbase = VV_FRAME0 + (fn << VV_FSHIFT);
    st.launches++; st.last_frame = fn;
    int n = 0, ok = 1;
    uint32_t pc = code;
    for (uint32_t i = 0; i < MAX_WORDS; i++) {
        uint32_t inst = rd_word(uc, pc, &ok); pc += 4;
        if (!ok) break;
        unsigned op = inst >> 28;
        uint32_t color = inst & 0x0FFFFFFFu;  /* GraphicsProcessor.v:154 {4'b0, COLOR28} */
        n++;
        if (op == VV_GOP_STOP) { st.cmds += n; return n; }
        if (op == VV_GOP_FILL) {               /* FrameFiller: whole 800x600 */
            for (int y = 0; y < H; y++) span(uc, y, 0, W - 1, color);
            continue;
        }
        if (op != VV_GOP_LINE && op != VV_GOP_ELIP && op != VV_GOP_RECT) {
            vv_eprintf("[gpu] fault: bad opcode %u (0x%08x) at 0x%08x\n", op, inst, pc - 4);
            st.faults++; st.cmds += n;
            return -2;                         /* GP_fault (GraphicsProcessor.v:188-189) */
        }
        uint32_t p0 = rd_word(uc, pc, &ok), p1 = rd_word(uc, pc + 4, &ok); pc += 8;
        /* gpcommands.vh:23-26 X=25:16, Y=9:0 (XVAR/YVAR flag fields ignored) */
        int x0 = (p0 >> 16) & 0x3FF, y0 = p0 & 0x3FF, x1 = (p1 >> 16) & 0x3FF, y1 = p1 & 0x3FF;
        if (op == VV_GOP_LINE) { do_line(uc, x0, y0, x1, y1, color); continue; }
        uint32_t fill = rd_word(uc, pc, &ok); pc += 4;
        if (!ok) break;
        if (op == VV_GOP_RECT) do_rect(uc, x0, y0, x1, y1, color, fill);
        else                   do_elip(uc, x0, y0, x1, y1, color, fill);
        if (verbose > 1) vv_eprintf("[gpu] op%u (%d,%d)-(%d,%d) c=%07x f=%08x\n", op, x0, y0, x1, y1, color, fill);
    }
    st.faults++; st.cmds += n;
    vv_eprintf("[gpu] fault: command list at 0x%08x ended without STOP (%d cmds)\n", code, n);
    return -1;
}
