#ifndef VV_TERM_H
#define VV_TERM_H
/* term.c - output streams and --color.
 * stdout carries ONLY the bytes the guest transmits on the UART (vv_uart_out); everything vvizard
 * prints for itself goes to stderr through vv_eprintf.  With color active, UART text is bold
 * phosphor green on stdout, and (when stderr is a terminal, or --color=always) the leading
 * "[tag]" of each stderr line and the register names in register tables are highlighted. */
#include <stdarg.h>

enum { VV_COLOR_NEVER, VV_COLOR_ALWAYS, VV_COLOR_AUTO };
int  vv_color_parse(const char *s);           /* "always|never|auto" (NULL = always); -1 if bad */
void vv_term_init(int mode);                  /* decide colors, set up buffering, atexit/signal resets */
extern int vv_color_out, vv_color_err;        /* color active on stdout / highlighting on stderr */

void vv_uart_out(int c);                      /* one guest UART TX byte -> stdout */
void vv_uart_flush(void);                     /* end the colored run and flush stdout */
void vv_term_tick(void);                      /* periodic: flush stdout if bytes are pending */

void vv_eprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vv_veprintf(const char *fmt, va_list ap);
/* Register name for a register table: "name" wrapped in the register highlight when active. */
const char *vv_creg(const char *name, char *buf, unsigned n);
#endif
