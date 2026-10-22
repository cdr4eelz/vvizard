/* gpu.c - GraphicsProcessor command interpreter (instant completion).
 * Evidence: hardware/src/gpu/gpcommands.vh, GraphicsProcessor.v, RectangleEngine.v,
 * ScanLineRunner.v, FrameFiller.v; software/common/graphics.h. */
#include "gpu.h"
#include "memmap.h"
#include "loader.h"
#include "display.h"
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
    uc_mem_write(uc, vv_canon(fbase + (uint32_t)y * VV_ROW_PITCH + (uint32_t)x0 * 4), buf, (size_t)(x1 - x0 + 1) * 4);
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
/* Scanline ellipse centered (xc,yc), radii a (x) and b (y): for each row offset dy the
 * half-width is the largest w with w^2*b^2 + dy^2*a^2 <= a^2*b^2; edge at +-w, fill between. */
static void do_elip(uc_engine *uc, int xc, int yc, int a, int b, uint32_t edge, uint32_t fill) {
    if (a == 0 || b == 0) { do_line(uc, xc - a, yc - b, xc + a, yc + b, edge); return; }
    int64_t A2 = (int64_t)a * a, B2 = (int64_t)b * b, AB = A2 * B2;
    int w = a, prev_w = 0;
    for (int dy = 0; dy <= b; dy++) {
        int64_t lim = AB - (int64_t)dy * dy * A2;
        while (w > 0 && (int64_t)w * w * B2 > lim) w--;
        /* Close gaps on steep sides: draw edge spans between this and previous half-width. */
        int outer = dy == 0 ? w : prev_w;
        for (int s = -1; s <= 1; s += 2) {
            int y = yc + s * dy;
            if (s == 1 && dy == 0) continue;
            edge_row(uc, y, xc - w, xc + w, edge, fill);
            if (outer > w + 1) { span(uc, y, xc - outer + 1, xc - w, edge); span(uc, y, xc + w, xc + outer - 1, edge); }
            if (dy == b && w > 0) span(uc, y, xc - w, xc + w, edge);   /* flat top/bottom cap */
        }
        prev_w = w;
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
            fprintf(stderr, "[gpu] fault: bad opcode %u (0x%08x) at 0x%08x\n", op, inst, pc - 4);
            break;
        }
        uint32_t p0 = rd_word(uc, pc, &ok), p1 = rd_word(uc, pc + 4, &ok); pc += 8;
        /* gpcommands.vh:23-26 X=25:16, Y=9:0 (XVAR/YVAR flag fields ignored) */
        int x0 = (p0 >> 16) & 0x3FF, y0 = p0 & 0x3FF, x1 = (p1 >> 16) & 0x3FF, y1 = p1 & 0x3FF;
        if (op == VV_GOP_LINE) { do_line(uc, x0, y0, x1, y1, color); continue; }
        uint32_t fill = rd_word(uc, pc, &ok); pc += 4;
        if (!ok) break;
        if (op == VV_GOP_RECT) do_rect(uc, x0, y0, x1, y1, color, fill);
        else                   do_elip(uc, x0, y0, x1, y1, color, fill);
        if (verbose > 1) fprintf(stderr, "[gpu] op%u (%d,%d)-(%d,%d) c=%07x f=%08x\n", op, x0, y0, x1, y1, color, fill);
    }
    st.faults++; st.cmds += n;
    fprintf(stderr, "[gpu] fault: command list at 0x%08x ended without STOP (%d cmds)\n", code, n);
    return -1;
}
