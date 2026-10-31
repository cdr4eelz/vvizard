#ifndef VV_DISPLAY_H
#define VV_DISPLAY_H
#include <stdint.h>
#include <stddef.h>
#include <unicorn/unicorn.h>
/* PixelFeeder model: 800x600, XRGB32 (R=23:16 G=15:8 B=7:0) big-endian words,
 * pixel addr = {4'h1, frame[5:0], y[9:0], x[9:0], 2'b00}  (PixelFeeder.v:187). */
#define VV_SCREEN_W 800
#define VV_SCREEN_H 600
#define VV_ROW_PITCH 4096u
#define VV_STRIP_H   76    /* status strip under the frame: LCD | status + recent keys */
int  vv_disp_init(int want_window);           /* 0 ok */
void vv_disp_pf_write(uc_engine *uc, uint32_t v); /* PF_FRAME write: select + redraw */
uint32_t vv_disp_frame(void);                  /* current frame# */
int  vv_disp_tick(uc_engine *uc);              /* periodic ~60Hz; returns 1 if window closed */
int  vv_disp_dump_ppm(uc_engine *uc, const char *path);
void vv_disp_close(void);
extern int vv_disp_keys_enabled;               /* window keys -> UART RX (default on) */
int  vv_disp_key_avail(void);
int  vv_disp_key_take(void);                   /* -1 if none */
void vv_disp_key_log(int c);                   /* show a key in the strip's recent-keys ticker */
int  vv_disp_screenshot(uc_engine *uc, const char *png); /* whole window incl. strip */
void vv_disp_status_dirty(void);               /* re-read the status lines at the next draw */
/* Provided by main.c: the two status lines shown in the strip. */
void vv_status_lines(uc_engine *uc, char *l1, size_t n1, char *l2, size_t n2);
#endif
