/* display.c - PixelFeeder frame viewer (raylib) + PPM dump, with a status strip under the
 * 800x600 frame: the 2x16 LCD (src/lcd.c), a status line and a recent-keys ticker. */
#include "display.h"
#include "memmap.h"
#include "lcd.h"
#include "lcdfont.h"
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int have_window;
static uint32_t cur_frame;              /* FRAME_BITS of last PF_FRAME write (reset 0) */
static Texture2D tex;
static uint8_t rgba[VV_SCREEN_W * VV_SCREEN_H * 4];
static double last_draw;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* gpcommands.vh:39  FRAME_BITS(F) = |F[31:28] ? F[27:22] : F[5:0] */
static uint32_t frame_bits(uint32_t f) { return (f >> 28) ? (f >> VV_FSHIFT) & VV_FNMASK : f & VV_FNMASK; }

/* Plain address order (no x^3 swizzle), per user decision. */
static void grab(uc_engine *uc) {
    static uint8_t row[VV_SCREEN_W * 4];
    uint32_t base = VV_FRAME0 + (cur_frame << VV_FSHIFT);
    for (int y = 0; y < VV_SCREEN_H; y++) {
        uint8_t *o = rgba + (size_t)y * VV_SCREEN_W * 4;
        if (uc_mem_read(uc, base + (uint32_t)y * VV_ROW_PITCH, row, sizeof row)) {
            for (int i = 0; i < VV_SCREEN_W * 4; i++) { o[i] = 0; }
            continue;
        }
        for (int x = 0; x < VV_SCREEN_W; x++) {   /* BE word: [X][R][G][B] */
            o[4*x+0] = row[4*x+1]; o[4*x+1] = row[4*x+2]; o[4*x+2] = row[4*x+3]; o[4*x+3] = 255;
        }
    }
}
/* ---- window keyboard -> UART RX queue ---- */
int vv_disp_keys_enabled = 1;
static unsigned char kq[256]; static unsigned kq_h, kq_t;
static void kq_put(int c) { unsigned n = (kq_t + 1) % sizeof kq; if (n != kq_h) { kq[kq_t] = (unsigned char)c; kq_t = n; } }
static void collect_keys(void) {
    if (!have_window || !vv_disp_keys_enabled) return;
    int c;
    while ((c = GetCharPressed()) > 0) if (c < 0x80) { kq_put(c); vv_disp_key_log(c); }   /* printable ASCII only */
    while ((c = GetKeyPressed()) > 0) switch (c) {
        case KEY_ENTER: case KEY_KP_ENTER: kq_put('\r'); vv_disp_key_log('\r'); break;
        case KEY_BACKSPACE: kq_put(0x08); vv_disp_key_log(0x08); break;
        case KEY_TAB: kq_put('\t'); vv_disp_key_log('\t'); break;
        case KEY_ESCAPE: kq_put(0x1b); vv_disp_key_log(0x1b); break;
        default: break; }
}

/* ---- recent keys ticker: newest at the right, older ones slide out to the left ---- */
#define KL_N 64
static unsigned char kl[KL_N]; static unsigned kl_n; static double kl_last;
void vv_disp_key_log(int c) { kl[kl_n++ % KL_N] = (unsigned char)c; kl_last = now(); }
static const char *key_name(unsigned c, char *b) {
    switch (c) {
    case ' ': return "SP"; case '\r': return "CR"; case '\n': return "LF"; case '\t': return "TAB";
    case 0x08: return "BS"; case 0x1b: return "ESC"; case 0x7f: return "DEL"; default: break; }
    if (c < 0x20) { b[0] = '^'; b[1] = (char)('@' + c); b[2] = 0; }
    else if (c >= 0x80) snprintf(b, 8, "%02X", c & 0xFFu);
    else { b[0] = (char)c; b[1] = 0; }
    return b;
}

/* ---- status strip ---- */
#define FS 10                                   /* raylib default font, native size */
static const Color LCD_BG = { 0x9A, 0xC2, 0x2E, 255 }, LCD_OFF = { 0x8C, 0xB2, 0x2A, 255 },
                   LCD_ON = { 0x1C, 0x2A, 0x0C, 255 }, BEZEL = { 0x14, 0x22, 0x14, 255 },
                   STRIP = { 0x20, 0x20, 0x24, 255 }, TXT = { 0xD8, 0xD8, 0xD8, 255 },
                   DIM = { 0x88, 0x88, 0x90, 255 }, WARN = { 0xFF, 0xB0, 0x40, 255 };
enum { DOT = 2, PITCH = 3, CH_W = 5 * PITCH, CH_H = 8 * PITCH, CH_GAP = 3, ROW_GAP = 4, PAD = 8,
       LCD_X = 6, LCD_Y = VV_SCREEN_H + (VV_STRIP_H - (2 * PAD + 2 * CH_H + ROW_GAP)) / 2,
       LCD_W = 2 * PAD + VV_LCD_COLS * (CH_W + CH_GAP) - CH_GAP, LCD_H = 2 * PAD + 2 * CH_H + ROW_GAP };
/* One HD44780 5x8 cell; A00 ROM roughly: 0x20-0x7F from lcdfont.h, 0x10-0x1F/0x80-0xA0 blank
 * in A00, CGRAM 0x00-0x0F (never loaded by LCDText2x16) and 0xA1-0xFF (katakana/Greek, not
 * drawn here) as a hollow box. */
static void draw_lcd_char(int x0, int y0, uint8_t ch) {
    uint8_t rows[8] = { 0 };
    if (ch >= 0x20 && ch < 0x80) memcpy(rows, vv_lcd_font[ch - 0x20], 7);
    else if (ch < 0x10 || ch > 0xA0) { rows[0] = rows[6] = 0x1F; for (int r = 1; r < 6; r++) rows[r] = 0x11; }
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 5; c++)
            DrawRectangle(x0 + c * PITCH, y0 + r * PITCH, DOT, DOT, rows[r] >> (4 - c) & 1 ? LCD_ON : LCD_OFF);
}
static void draw_strip(uc_engine *uc) {
    DrawRectangle(0, VV_SCREEN_H, VV_SCREEN_W, VV_STRIP_H, STRIP);
    DrawRectangle(LCD_X - 3, LCD_Y - 3, LCD_W + 6, LCD_H + 6, BEZEL);
    DrawRectangle(LCD_X, LCD_Y, LCD_W, LCD_H, LCD_BG);
    for (int i = 0; i < VV_LCD_CHARS; i++)
        draw_lcd_char(LCD_X + PAD + (i % VV_LCD_COLS) * (CH_W + CH_GAP),
                      LCD_Y + PAD + (i / VV_LCD_COLS) * (CH_H + ROW_GAP), vv_lcd_get((unsigned)i));
    int x = LCD_X + LCD_W + 12, w = VV_SCREEN_W - x - 6, y = VV_SCREEN_H + 6;
    char l1[160], l2[160];
    vv_status_lines(uc, l1, sizeof l1, l2, sizeof l2);
    DrawText(l1, x, y, FS, TXT);
    {   /* second line word by word: PAUSED stands out */
        int wx = x, sp = MeasureText("a", FS);
        for (char *p = l2; *p; ) {
            if (*p == ' ') { wx += sp; p++; continue; }
            char *e = strchr(p, ' '), save = 0; if (e) { save = *e; *e = 0; }
            DrawText(p, wx, y + 16, FS, strcmp(p, "PAUSED") ? TXT : WARN);
            wx += MeasureText(p, FS) + 1;
            if (!e) break;
            *e = save; p = e;
        }
    }
    /* ticker */
    int ty = y + 36, th = VV_SCREEN_H + VV_STRIP_H - 6 - ty;
    DrawText("keys", x, ty + (th - FS) / 2, FS, DIM);
    int kx = x + MeasureText("keys", FS) + 8, kw = x + w - kx;
    DrawRectangle(kx, ty, kw, th, (Color){ 0x16, 0x16, 0x18, 255 });
    BeginScissorMode(kx, ty, kw, th);
    double t = (now() - kl_last) / 0.12; float slide = t < 1 ? (float)(1 - t) : 0;
    int right = kx + kw - 4, first = 1;
    unsigned n = kl_n < KL_N ? kl_n : KL_N;
    for (unsigned i = 0; i < n && right > kx; i++) {
        char b[8]; const char *s = key_name(kl[(kl_n - 1 - i) % KL_N], b);
        int cw = MeasureText(s, FS) + 8;
        if (first) { right += (int)(slide * (cw + 3)); first = 0; }
        int cx = right - cw;
        DrawRectangle(cx, ty + 3, cw, th - 6, i == 0 ? (Color){ 0x50, 0x60, 0x30, 255 } : (Color){ 0x38, 0x38, 0x40, 255 });
        DrawText(s, cx + 4, ty + (th - FS) / 2, FS, i == 0 ? RAYWHITE : TXT);
        right = cx - 3;
    }
    EndScissorMode();
}
static void render(uc_engine *uc) {
    ClearBackground(BLACK); DrawTexture(tex, 0, 0, WHITE); draw_strip(uc);
}
int vv_disp_key_avail(void) { return kq_h != kq_t; }
int vv_disp_key_take(void) { if (kq_h == kq_t) return -1; int c = kq[kq_h]; kq_h = (kq_h + 1) % sizeof kq; return c; }
static void draw(uc_engine *uc) {
    if (!have_window) return;
    grab(uc);
    UpdateTexture(tex, rgba);
    BeginDrawing(); render(uc); EndDrawing();
    collect_keys();                         /* EndDrawing polled; queues reset next poll */
    last_draw = now();
}
int vv_disp_init(int want_window) {
    if (!want_window) return 0;
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(VV_SCREEN_W, VV_SCREEN_H + VV_STRIP_H, "vvizard");
    if (!IsWindowReady()) { fprintf(stderr, "[disp] cannot open window; continuing headless\n"); return -1; }
    Image img = { rgba, VV_SCREEN_W, VV_SCREEN_H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    tex = LoadTextureFromImage(img);
    SetExitKey(KEY_NULL);                   /* Esc goes to UART, not window close */
    have_window = 1; last_draw = 0;
    return 0;
}
void vv_disp_pf_write(uc_engine *uc, uint32_t v) {
    cur_frame = frame_bits(v);              /* applied immediately (HW: at end of scan) */
    draw(uc);
}
uint32_t vv_disp_frame(void) { return cur_frame; }
int vv_disp_tick(uc_engine *uc) {
    if (!have_window) return 0;
    if (now() - last_draw >= 1.0 / 60) draw(uc);      /* EndDrawing polls events + collects */
    else { PollInputEvents(); collect_keys(); }
    return WindowShouldClose();
}
int vv_disp_dump_ppm(uc_engine *uc, const char *path) {
    grab(uc);
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", VV_SCREEN_W, VV_SCREEN_H);
    for (int i = 0; i < VV_SCREEN_W * VV_SCREEN_H; i++) fwrite(rgba + 4*i, 1, 3, f);
    fclose(f);
    fprintf(stderr, "[disp] dumped frame %u (0x%08x) to %s\n", cur_frame, VV_FRAME0 + (cur_frame << VV_FSHIFT), path);
    return 0;
}
/* Whole window (frame + strip) as PNG: render offscreen so the read-back is well defined. */
int vv_disp_screenshot(uc_engine *uc, const char *path) {
    if (!have_window) { fprintf(stderr, "[disp] --screenshot needs the window\n"); return -1; }
    grab(uc); UpdateTexture(tex, rgba);
    RenderTexture2D rt = LoadRenderTexture(VV_SCREEN_W, VV_SCREEN_H + VV_STRIP_H);
    BeginTextureMode(rt); render(uc); EndTextureMode();
    Image img = LoadImageFromTexture(rt.texture); ImageFlipVertical(&img);
    int ok = ExportImage(img, path);
    UnloadImage(img); UnloadRenderTexture(rt);
    fprintf(stderr, "[disp] screenshot %s %s\n", path, ok ? "written" : "FAILED");
    return ok ? 0 : -1;
}
void vv_disp_close(void) { if (have_window) { UnloadTexture(tex); CloseWindow(); have_window = 0; } }
