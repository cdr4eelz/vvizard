/* lcd.c - 2x16 LCD text buffer model (see lcd.h). */
#include "term.h"
#include "lcd.h"

/* LCDText2x16.v: parameter [255:0] INIT_TEXT = {32{8'h20}}; leftmost char = high byte. */
static const char vv_lcd_init_text[VV_LCD_CHARS + 1] = "                                ";
static uint8_t buf[VV_LCD_CHARS];
static uint64_t nwrites;

void vv_lcd_reset(void) { for (unsigned i = 0; i < VV_LCD_CHARS; i++) buf[i] = (uint8_t)vv_lcd_init_text[i]; }
void vv_lcd_write(unsigned idx, uint8_t ch) { buf[idx % VV_LCD_CHARS] = ch; nwrites++; }
uint8_t vv_lcd_get(unsigned idx) { return buf[idx % VV_LCD_CHARS]; }
uint64_t vv_lcd_writes(void) { return nwrites; }
void vv_lcd_dump(void) {
    for (unsigned r = 0; r < VV_LCD_ROWS; r++) {
        char t[VV_LCD_COLS + 1], h[3 * VV_LCD_COLS + 1];
        for (unsigned c = 0; c < VV_LCD_COLS; c++) { uint8_t ch = buf[r * VV_LCD_COLS + c];
            t[c] = ch >= 0x20 && ch < 0x7F ? (char)ch : '.'; snprintf(h + 3 * c, 4, " %02x", ch); }
        t[VV_LCD_COLS] = 0;
        vv_eprintf("[lcd] %u |%s| hex%s\n", r + 1, t, h);
    }
    vv_eprintf("[lcd] writes: %llu\n", (unsigned long long)nwrites);
}
