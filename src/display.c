/* display.c - PixelFeeder frame viewer (raylib) + PPM dump. */
#include "display.h"
#include "memmap.h"
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
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
static void draw(uc_engine *uc) {
    if (!have_window) return;
    grab(uc);
    UpdateTexture(tex, rgba);
    BeginDrawing(); ClearBackground(BLACK); DrawTexture(tex, 0, 0, WHITE); EndDrawing();
    last_draw = now();
}
int vv_disp_init(int want_window) {
    if (!want_window) return 0;
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(VV_SCREEN_W, VV_SCREEN_H, "VVIZARD - PixelFeeder frame 0");
    if (!IsWindowReady()) { fprintf(stderr, "[disp] cannot open window; continuing headless\n"); return -1; }
    Image img = { rgba, VV_SCREEN_W, VV_SCREEN_H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    tex = LoadTextureFromImage(img);
    have_window = 1; last_draw = 0;
    return 0;
}
void vv_disp_pf_write(uc_engine *uc, uint32_t v) {
    cur_frame = frame_bits(v);              /* applied immediately (HW: at end of scan) */
    if (have_window) { char t[64]; snprintf(t, sizeof t, "VVIZARD - PixelFeeder frame %u", cur_frame); SetWindowTitle(t); }
    draw(uc);
}
uint32_t vv_disp_frame(void) { return cur_frame; }
int vv_disp_tick(uc_engine *uc) {
    if (!have_window) return 0;
    if (now() - last_draw >= 1.0 / 60) draw(uc);
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
void vv_disp_close(void) { if (have_window) { UnloadTexture(tex); CloseWindow(); have_window = 0; } }
