/* display.c - PixelFeeder frame viewer (raylib) + PPM dump, with a status strip under the
 * 800x600 frame: the 2x16 LCD (src/lcd.c), a status line and a recent-keys ticker. */
#include "term.h"
#include "display.h"
#include "memmap.h"
#include "lcd.h"
#include "lcdfont.h"
#include "gpu.h"
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>

static int have_window;
static uint32_t cur_frame;              /* displayed frame (reset 0); hw.c switches it at frame end */
static Texture2D tex;
static uint8_t rgba[VV_SCREEN_W * VV_SCREEN_H * 4];
static double last_draw, last_poll;
#define POLL_PERIOD (1.0 / 500)             /* input poll between draws: at most every 2 ms host time */

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* gpcommands.vh:39  FRAME_BITS(F) = |F[31:28] ? F[27:22] : F[5:0] */
static uint32_t frame_bits(uint32_t f) { return (f >> 28) ? (f >> VV_FSHIFT) & VV_FNMASK : f & VV_FNMASK; }

/* Big-endian XRGB row -> RGBA */
static void swizzle_row(uint8_t *o, const uint8_t *row) {
    for (int x = 0; x < VV_SCREEN_W; x++) {   /* BE word: [X][R][G][B] */
        o[4*x+0] = row[4*x+1]; o[4*x+1] = row[4*x+2]; o[4*x+2] = row[4*x+3]; o[4*x+3] = 255;
    }
}
/* Plain address order (no x^3 swizzle), per user decision.  out: RGBA 800x600. */
static void grab(uc_engine *uc, uint8_t *out) {
    static uint8_t rowbuf[VV_SCREEN_W * 4];
    uint32_t base = VV_FRAME0 + (cur_frame << VV_FSHIFT);
    vv_gpu_settle(cur_frame);               /* never read a frame the GPU worker is drawing */
    for (int y = 0; y < VV_SCREEN_H; y++) {
        uint8_t *o = out + (size_t)y * VV_SCREEN_W * 4;
        uint32_t a = base + (uint32_t)y * VV_ROW_PITCH;
        const uint8_t *row = vv_ram(a, sizeof rowbuf);     /* DDR host buffer, no copy */
        if (!row && !uc_mem_read(uc, a, rowbuf, sizeof rowbuf)) row = rowbuf;
        if (!row) {
            for (int i = 0; i < VV_SCREEN_W * 4; i++) { o[i] = 0; }
            continue;
        }
        swizzle_row(o, row);
    }
}
/* ---- window keyboard -> UART RX queue ----
 * SPSC ring: the display (main) thread produces (collect_keys), the emulator consumes
 * (vv_disp_key_take via main.c's vv_host_rx_*).  Single-threaded runs use it the same way. */
int vv_disp_keys_enabled = 1;
static unsigned char kq[256]; static _Atomic unsigned kq_h, kq_t;
static void kq_put(int c) {
    unsigned t = atomic_load_explicit(&kq_t, memory_order_relaxed), n = (t + 1) % sizeof kq;
    if (n == atomic_load_explicit(&kq_h, memory_order_acquire)) return;     /* full: drop */
    kq[t] = (unsigned char)c; atomic_store_explicit(&kq_t, n, memory_order_release);
}
/* Esc ends the run (VV_DISP_ESC from vv_disp_tick): a terminal on a real UART (screen etc.) can't
 * send a lone Esc cleanly, so it is not forwarded to the guest.  Checked even with --no-window-keys. */
static atomic_int esc_pressed, win_closed;
static void kl_add(int c);
static void collect_keys(void) {
    if (!have_window) return;
    int c, keys = vv_disp_keys_enabled;
    while ((c = GetCharPressed()) > 0) if (keys && c < 0x80) { kq_put(c); kl_add(c); }   /* printable ASCII only */
    while ((c = GetKeyPressed()) > 0) {
        if (c == KEY_ESCAPE) { atomic_store(&esc_pressed, 1); continue; }
        if (!keys) continue;
        switch (c) {
        case KEY_ENTER: case KEY_KP_ENTER: kq_put('\r'); kl_add('\r'); break;
        case KEY_BACKSPACE: kq_put(0x08); kl_add(0x08); break;
        case KEY_TAB: kq_put('\t'); kl_add('\t'); break;
        default: break; }
    }
}

/* ---- threaded display (docs/MULTITHREADING-PLAN.md sections 4-5) ----
 * vv_disp_threaded: the window is drawn by the main thread (vv_disp_main_loop) while the emulator
 * runs uc_emu_start on its own thread.  The emulator never calls raylib: at a PixelFeeder frame end
 * (and on the vv_disp_tick redraw timer) it copies the displayed frame's visible rows, raw
 * big-endian, into the back slot of a triple buffer together with a snapshot of the status lines and
 * the LCD, and swaps it with the middle slot (atomic exchange; never waits).  Main swaps the middle
 * slot with its front slot when it is fresh, swizzles and draws.  Bytes the emulator feeds to UART
 * RX from --uart-in / the script / stdin reach the key ticker through a second SPSC ring. */
int vv_disp_threaded;
typedef struct {
    uint8_t px[VV_SCREEN_H][VV_SCREEN_W * 4];
    char l1[160], l2[160];
    uint8_t lcd[VV_LCD_CHARS];
    uint32_t frame;
} disp_slot;
static disp_slot slots[3];
#define SLOT_FRESH 4u
static _Atomic unsigned tb_mid = 1;         /* middle slot index | SLOT_FRESH when not yet shown */
static unsigned tb_back = 0, tb_front = 2;  /* back: emulator only; front: main only */
static atomic_int emu_done;
static pthread_mutex_t pub_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pub_cv;              /* CLOCK_MONOTONIC where available (vv_disp_init) */
static void wake_main(void) { pthread_mutex_lock(&pub_mx); pthread_cond_signal(&pub_cv); pthread_mutex_unlock(&pub_mx); }
static unsigned char klq[256]; static _Atomic unsigned klq_h, klq_t;   /* emulator -> main key log */

/* ---- recent keys ticker: newest at the right, older ones slide out to the left ---- */
#define KL_N 64
static unsigned char kl[KL_N]; static unsigned kl_n; static double kl_last;
static void kl_add(int c) { kl[kl_n++ % KL_N] = (unsigned char)c; kl_last = now(); }
void vv_disp_key_log(int c) {
    if (!vv_disp_threaded) { kl_add(c); return; }
    unsigned t = atomic_load_explicit(&klq_t, memory_order_relaxed), n = (t + 1) % sizeof klq;
    if (n == atomic_load_explicit(&klq_h, memory_order_acquire)) return;
    klq[t] = (unsigned char)c; atomic_store_explicit(&klq_t, n, memory_order_release);
}
static void klq_drain(void) {
    unsigned h = atomic_load_explicit(&klq_h, memory_order_relaxed);
    while (h != atomic_load_explicit(&klq_t, memory_order_acquire)) {
        kl_add(klq[h]); h = (h + 1) % sizeof klq; atomic_store_explicit(&klq_h, h, memory_order_release); }
}
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
#define STATUS_HZ 4
static int status_dirty;
void vv_disp_status_dirty(void) { status_dirty = 1; }
/* The status text (instruction count etc.) is re-formatted at most STATUS_HZ times a second (and
 * when vv_disp_status_dirty() was called), so the numbers stay readable.  Runs on the emulator
 * (threaded: at a publish) or on the only thread. */
static char st_l1[160], st_l2[160]; static double st_at = -1;
static void status_refresh(uc_engine *uc, double t_now) {
    if (status_dirty || st_at < 0 || t_now - st_at >= 1.0 / STATUS_HZ) {
        vv_status_lines(uc, st_l1, sizeof st_l1, st_l2, sizeof st_l2); st_at = t_now; status_dirty = 0; }
}
static void draw_strip(const char *l1, const char *l2, const uint8_t *lcd) {
    DrawRectangle(0, VV_SCREEN_H, VV_SCREEN_W, VV_STRIP_H, STRIP);
    DrawRectangle(LCD_X - 3, LCD_Y - 3, LCD_W + 6, LCD_H + 6, BEZEL);
    DrawRectangle(LCD_X, LCD_Y, LCD_W, LCD_H, LCD_BG);
    for (int i = 0; i < VV_LCD_CHARS; i++)
        draw_lcd_char(LCD_X + PAD + (i % VV_LCD_COLS) * (CH_W + CH_GAP),
                      LCD_Y + PAD + (i / VV_LCD_COLS) * (CH_H + ROW_GAP), lcd[i]);
    int x = LCD_X + LCD_W + 12, w = VV_SCREEN_W - x - 6, y = VV_SCREEN_H + 6;
    /* drawing the strip costs ~0.4 ms per frame (NOTES.md "Window cost") */
    double t_now = now();
    char l2w[160]; snprintf(l2w, sizeof l2w, "%s", l2);
    DrawText(l1, x, y, FS, TXT);
    {   /* second line word by word: PAUSED stands out */
        int wx = x, sp = MeasureText("a", FS);
        for (char *p = l2w; *p; ) {
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
    double t = (t_now - kl_last) / 0.12; float slide = t < 1 ? (float)(1 - t) : 0;
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
static void render(uc_engine *uc) {     /* single thread: status from the emulator state */
    uint8_t lcd[VV_LCD_CHARS];
    for (int i = 0; i < VV_LCD_CHARS; i++) lcd[i] = vv_lcd_get((unsigned)i);
    status_refresh(uc, now());
    ClearBackground(BLACK); DrawTexture(tex, 0, 0, WHITE); draw_strip(st_l1, st_l2, lcd);
}
int vv_disp_key_avail(void) {
    return atomic_load_explicit(&kq_h, memory_order_relaxed) != atomic_load_explicit(&kq_t, memory_order_acquire); }
int vv_disp_key_take(void) {
    unsigned h = atomic_load_explicit(&kq_h, memory_order_relaxed);
    if (h == atomic_load_explicit(&kq_t, memory_order_acquire)) return -1;
    int c = kq[h]; atomic_store_explicit(&kq_h, (h + 1) % sizeof kq, memory_order_release); return c;
}
static void draw(uc_engine *uc) {
    if (!have_window) return;
    grab(uc, rgba);
    UpdateTexture(tex, rgba);
    BeginDrawing(); render(uc); EndDrawing();
    collect_keys();                         /* EndDrawing polled; queues reset next poll */
    last_draw = last_poll = now();
}
static void raylib_log(int level, const char *fmt, va_list ap) {   /* raylib logs to stdout by default */
    (void)level; vv_term_lock(); vv_eprintf("[raylib] "); vv_veprintf(fmt, ap); vv_eprintf("\n"); vv_term_unlock();
}
int vv_disp_init(int want_window) {
    pthread_condattr_t ca; pthread_condattr_init(&ca);
#ifndef __APPLE__
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&pub_cv, &ca); pthread_condattr_destroy(&ca);
    if (!want_window) return 0;
    SetTraceLogCallback(raylib_log);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(VV_SCREEN_W, VV_SCREEN_H + VV_STRIP_H, "vvizard");
    if (!IsWindowReady()) { vv_eprintf("[disp] cannot open window; continuing headless\n"); return -1; }
    Image img = { rgba, VV_SCREEN_W, VV_SCREEN_H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    tex = LoadTextureFromImage(img);
    SetExitKey(KEY_NULL);                   /* raylib's default exit key is Esc; vvizard handles Esc itself
                                             * (collect_keys) so it can report it and run the normal shutdown */
    have_window = 1; last_draw = 0;
    return 0;
}
/* Emulator thread (threaded mode): copy the displayed frame + status into the back slot and publish. */
static double last_pub;
static void publish(uc_engine *uc) {
    disp_slot *sl = &slots[tb_back];
    uint32_t base = VV_FRAME0 + (cur_frame << VV_FSHIFT);
    vv_gpu_settle(cur_frame);               /* (shows the finished draw; the guest is not affected) */
    for (int y = 0; y < VV_SCREEN_H; y++) {
        uint32_t a = base + (uint32_t)y * VV_ROW_PITCH;
        const uint8_t *row = vv_ram(a, VV_SCREEN_W * 4);
        if (row) memcpy(sl->px[y], row, VV_SCREEN_W * 4);
        else if (uc_mem_read(uc, a, sl->px[y], VV_SCREEN_W * 4)) memset(sl->px[y], 0, VV_SCREEN_W * 4);
    }
    double t = now();
    status_refresh(uc, t);
    memcpy(sl->l1, st_l1, sizeof sl->l1); memcpy(sl->l2, st_l2, sizeof sl->l2);
    for (int i = 0; i < VV_LCD_CHARS; i++) sl->lcd[i] = vv_lcd_get((unsigned)i);
    sl->frame = cur_frame;
    tb_back = atomic_exchange_explicit(&tb_mid, tb_back | SLOT_FRESH, memory_order_acq_rel) & 3u;
    last_pub = t;
    wake_main();
}
void vv_disp_pf_write(uc_engine *uc, uint32_t v) {
    cur_frame = frame_bits(v);              /* called by hw.c at every PixelFeeder frame end (and for an
                                             * immediate PF_FRAME write: --legacy-io, --pf-period 0) */
    if (vv_disp_threaded) publish(uc); else draw(uc);
}
uint32_t vv_disp_frame(void) { return cur_frame; }
/* Called every 4096 instructions (main.c): redraw when nothing drew for vv_disp_redraw_period (1/60 s;
 * with --frame-clock=wall the ~60/s PixelFeeder frame ends draw, so main.c makes this only a fallback);
 * in between, poll window input by host time (POLL_PERIOD) rather than on every call, which at 20-40M
 * instr/s was thousands of polls a second. */
double vv_disp_redraw_period = 1.0 / 60;
int vv_disp_has_window(void) { return have_window; }
int vv_disp_tick(uc_engine *uc) {
    if (!have_window) return 0;
    double t = now();
    if (vv_disp_threaded) {                 /* emulator thread: no raylib; flags set by the main thread */
        if (t - last_pub >= vv_disp_redraw_period) publish(uc);
        if (atomic_exchange(&esc_pressed, 0)) return VV_DISP_ESC;
        return atomic_load(&win_closed) ? VV_DISP_CLOSED : 0;
    }
    if (t - last_draw >= vv_disp_redraw_period) draw(uc);          /* EndDrawing polls events + collects */
    else if (t - last_poll >= POLL_PERIOD) { PollInputEvents(); collect_keys(); last_poll = t; }
    if (atomic_exchange(&esc_pressed, 0)) return VV_DISP_ESC;
    return WindowShouldClose() ? VV_DISP_CLOSED : 0;
}
/* Emulator thread, after uc_emu_start returned: publish the final frame, then let main finish. */
void vv_disp_emu_done(uc_engine *uc) {
    if (vv_disp_threaded) publish(uc);
    atomic_store(&emu_done, 1); wake_main();
}
static int draw_fresh(void) {               /* main: draw the middle slot if it is new */
    if (!(atomic_load_explicit(&tb_mid, memory_order_acquire) & SLOT_FRESH)) return 0;
    tb_front = atomic_exchange_explicit(&tb_mid, tb_front, memory_order_acq_rel) & 3u;
    const disp_slot *sl = &slots[tb_front];
    for (int y = 0; y < VV_SCREEN_H; y++) swizzle_row(rgba + (size_t)y * VV_SCREEN_W * 4, sl->px[y]);
    klq_drain();
    UpdateTexture(tex, rgba);
    BeginDrawing(); ClearBackground(BLACK); DrawTexture(tex, 0, 0, WHITE); draw_strip(sl->l1, sl->l2, sl->lcd); EndDrawing();
    collect_keys();                          /* EndDrawing polled */
    return 1;
}
static void wait_publish(void) {            /* up to POLL_PERIOD for the emulator's next publish */
    pthread_mutex_lock(&pub_mx);
    if (!(atomic_load_explicit(&tb_mid, memory_order_acquire) & SLOT_FRESH) && !atomic_load(&emu_done)) {
#ifdef __APPLE__
        struct timespec rel = { 0, (long)(POLL_PERIOD * 1e9) };
        pthread_cond_timedwait_relative_np(&pub_cv, &pub_mx, &rel);
#else
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += (long)(POLL_PERIOD * 1e9);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&pub_cv, &pub_mx, &ts);
#endif
    }
    pthread_mutex_unlock(&pub_mx);
}
/* Main thread while the emulator thread runs: input, and a redraw for each fresh slot. */
void vv_disp_main_loop(void) {
    double lp = 0;
    while (!atomic_load(&emu_done)) {
        wait_publish();
        double t = now();
        if (draw_fresh()) lp = t;
        else if (t - lp >= POLL_PERIOD) { PollInputEvents(); collect_keys(); lp = t; }
        if (WindowShouldClose()) atomic_store(&win_closed, 1);
    }
    draw_fresh();                            /* the final frame (vv_disp_emu_done) */
    klq_drain();
}
int vv_disp_dump_ppm(uc_engine *uc, const char *path) {   /* emulator thread: own buffer, not rgba */
    static uint8_t px[VV_SCREEN_W * VV_SCREEN_H * 4];
    grab(uc, px);
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", VV_SCREEN_W, VV_SCREEN_H);
    for (int i = 0; i < VV_SCREEN_W * VV_SCREEN_H; i++) fwrite(px + 4*i, 1, 3, f);
    fclose(f);
    vv_eprintf("[disp] dumped frame %u (0x%08x) to %s\n", cur_frame, VV_FRAME0 + (cur_frame << VV_FSHIFT), path);
    return 0;
}
/* Whole window (frame + strip) as PNG: render offscreen so the read-back is well defined. */
int vv_disp_screenshot(uc_engine *uc, const char *path) {
    if (!have_window) { vv_eprintf("[disp] --screenshot needs the window\n"); return -1; }
    grab(uc, rgba); UpdateTexture(tex, rgba); status_dirty = 1;
    RenderTexture2D rt = LoadRenderTexture(VV_SCREEN_W, VV_SCREEN_H + VV_STRIP_H);
    BeginTextureMode(rt); render(uc); EndTextureMode();
    Image img = LoadImageFromTexture(rt.texture); ImageFlipVertical(&img);
    int ok = ExportImage(img, path);
    UnloadImage(img); UnloadRenderTexture(rt);
    vv_eprintf("[disp] screenshot %s %s\n", path, ok ? "written" : "FAILED");
    return ok ? 0 : -1;
}
void vv_disp_close(void) { if (have_window) { UnloadTexture(tex); CloseWindow(); have_window = 0; } }
