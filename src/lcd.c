/* lcd.c - 2x16 LCD text buffer model (see lcd.h). */
#include "lcd.h"

/* LCDText2x16.v: parameter [255:0] INIT_TEXT = {32{8'h20}}; leftmost char = high byte. */
static const char vv_lcd_init_text[VV_LCD_CHARS + 1] = "                                ";
static uint8_t buf[VV_LCD_CHARS];
static uint64_t nwrites;

void vv_lcd_reset(void) { for (unsigned i = 0; i < VV_LCD_CHARS; i++) buf[i] = (uint8_t)vv_lcd_init_text[i]; }
void vv_lcd_write(unsigned idx, uint8_t ch) { buf[idx % VV_LCD_CHARS] = ch; nwrites++; }
uint8_t vv_lcd_get(unsigned idx) { return buf[idx % VV_LCD_CHARS]; }
uint64_t vv_lcd_writes(void) { return nwrites; }
void vv_lcd_dump(FILE *f) {
    for (unsigned r = 0; r < VV_LCD_ROWS; r++) {
        fprintf(f, "[lcd] %u |", r + 1);
        for (unsigned c = 0; c < VV_LCD_COLS; c++) { uint8_t ch = buf[r * VV_LCD_COLS + c]; fputc(ch >= 0x20 && ch < 0x7F ? ch : '.', f); }
        fprintf(f, "| hex");
        for (unsigned c = 0; c < VV_LCD_COLS; c++) fprintf(f, " %02x", buf[r * VV_LCD_COLS + c]);
        fputc('\n', f);
    }
    fprintf(f, "[lcd] writes: %llu\n", (unsigned long long)nwrites);
}
