#ifndef VV_LCD_H
#define VV_LCD_H
#include <stdint.h>
#include <stdio.h>
/* 2x16 character LCD text buffer (hardware/src/base_io/LCDText2x16.v via cpu/MemMapIO.v).
 * MMIO word i (0..31) at 0x80000100 + 4*i holds character i: 0..15 line 1, 16..31 line 2,
 * left to right. The character is dina[7:0] of the (lane-placed) store data. Write-only:
 * reads of the window return 0. Reset contents = INIT_TEXT (LCDText2x16 default: 32 spaces). */
#define VV_LCD_COLS  16
#define VV_LCD_ROWS  2
#define VV_LCD_CHARS (VV_LCD_COLS * VV_LCD_ROWS)
void     vv_lcd_reset(void);                  /* fill from INIT_TEXT */
void     vv_lcd_write(unsigned idx, uint8_t ch);
uint8_t  vv_lcd_get(unsigned idx);
uint64_t vv_lcd_writes(void);                 /* number of LCD stores so far */
void     vv_lcd_dump(void);                   /* stderr: "[lcd] 1|...|" text + hex, both lines */
#endif
