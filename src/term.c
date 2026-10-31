/* term.c - stdout/stderr split and --color (see term.h). */
#include "term.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

int vv_color_out, vv_color_err;
static int out_tty;                           /* stdout is a terminal: flush every byte */
static int out_active;                        /* the UART color is switched on in stdout */
static int out_dirty;                         /* bytes buffered in stdout */
static int err_bol = 1;                       /* stderr is at the beginning of a line */
static const char *uart_on = "\033[1;32m";    /* 8-color fallback; 256-color in vv_term_init */
#define RESET "\033[0m"

int vv_color_parse(const char *s) {
    if (!s || !strcmp(s, "always") || !strcmp(s, "yes") || !strcmp(s, "on")) return VV_COLOR_ALWAYS;
    if (!strcmp(s, "never") || !strcmp(s, "no") || !strcmp(s, "off")) return VV_COLOR_NEVER;
    if (!strcmp(s, "auto")) return VV_COLOR_AUTO;
    return -1;
}

static void at_exit(void) {
    vv_uart_flush();
    if (vv_color_err && !err_bol) fputs(RESET, stderr);
    fflush(stderr);
}
static void on_signal(int sig) {               /* async-signal-safe: restore the terminal, re-raise */
    if (out_active) { ssize_t r = write(1, RESET, sizeof RESET - 1); (void)r; }
    if (vv_color_err) { ssize_t r = write(2, RESET, sizeof RESET - 1); (void)r; }
    signal(sig, SIG_DFL); raise(sig);
}

void vv_term_init(int mode) {
    out_tty = isatty(1);
    int err_tty = isatty(2);
    const char *nc = getenv("NO_COLOR"), *term = getenv("TERM"), *ct = getenv("COLORTERM");
    if (mode == VV_COLOR_AUTO)
        vv_color_out = out_tty && !(nc && *nc) && !(term && !strcmp(term, "dumb"));
    else vv_color_out = mode == VV_COLOR_ALWAYS;
    vv_color_err = vv_color_out && (mode == VV_COLOR_ALWAYS || err_tty);
    if ((term && strstr(term, "256color")) || (ct && *ct) || (term && strstr(term, "direct")))
        uart_on = "\033[1;38;5;46m";             /* bold phosphor green (xterm 256-color #46) */
    /* stdout: our own buffer, flushed at newline, before stderr output, periodically, and per byte on a tty */
    static char obuf[1 << 14];
    setvbuf(stdout, obuf, _IOFBF, sizeof obuf);
    atexit(at_exit);
    if (vv_color_out || vv_color_err) {
        int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
        for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
            void (*old)(int) = signal(sigs[i], on_signal);
            if (old == SIG_IGN) signal(sigs[i], SIG_IGN);   /* keep e.g. nohup's ignore */
        }
    }
}

void vv_uart_flush(void) {
    if (out_active) { fputs(RESET, stdout); out_active = 0; out_dirty = 1; }
    if (out_dirty) { fflush(stdout); out_dirty = 0; }
}
/* Flush buffered UART text but stay in the colored run (a run ends at a line end, stderr output or exit). */
static void out_flush(void) { if (out_dirty) { fflush(stdout); out_dirty = 0; } }
void vv_term_tick(void) { out_flush(); }

void vv_uart_out(int c) {
    if (vv_color_out) {
        if (c == '\n' || c == '\r') {            /* reset before line ends so nothing bleeds */
            if (out_active) { fputs(RESET, stdout); out_active = 0; }
        } else if (!out_active) { fputs(uart_on, stdout); out_active = 1; }
    }
    putc(c, stdout); out_dirty = 1;
    if (out_tty || c == '\n') out_flush();
}

/* ---- stderr ---- */
static const struct { const char *tag, *sgr; } tag_colors[] = {
    { "stop",   "1;31" }, { "quirk", "1;33" }, { "irq",    "1;35" }, { "mmio",   "34"   },
    { "trace",  "36"   }, { "hw",    "1;36" }, { "run",    "1;32" }, { "load",   "32"   },
    { "bios",   "32"   }, { "ddr",   "32"   }, { "fault",  "1;91" }, { "warn",   "1;93" },
    { "gpu",    "94"   }, { "disp",  "96"   }, { "lcd",    "95"   }, { "script", "92"   },
    { "expect", "1;97" }, { "raylib","90"   },
};
static const char *tag_sgr(const char *t, size_t n) {
    for (unsigned i = 0; i < sizeof tag_colors / sizeof tag_colors[0]; i++)
        if (strlen(tag_colors[i].tag) == n && !strncmp(tag_colors[i].tag, t, n)) return tag_colors[i].sgr;
    return "1";
}
/* Copy s to stderr, highlighting "[tag]" at the start of a line and the bracketed run markers. */
static void err_colored(const char *s) {
    static const char *const marks[][2] = { { " [guest bug]", "1;31" }, { " [emulated]", "33" },
                                            { " [not emulated: standard MIPS]", "2" },
                                            { " PASS", "1;32" }, { " FAIL", "1;31" } };
    while (*s) {
        if (err_bol && *s == '[') {
            const char *e = s + 1;
            while ((*e >= 'a' && *e <= 'z') || (*e >= '0' && *e <= '9') || *e == '-') e++;
            if (*e == ']' && e > s + 1) {
                fprintf(stderr, "\033[%sm%.*s" RESET, tag_sgr(s + 1, e - s - 1), (int)(e - s + 1), s);
                s = e + 1; err_bol = 0; continue;
            }
        }
        int hit = 0;
        for (unsigned i = 0; i < sizeof marks / sizeof marks[0] && !hit; i++) {
            size_t n = strlen(marks[i][0]);
            if (!strncmp(s, marks[i][0], n) && (s[n] == '\n' || s[n] == 0 || s[n] == ' ')) {
                fprintf(stderr, " \033[%sm%s" RESET, marks[i][1], marks[i][0] + 1); s += n; hit = 1; }
        }
        if (hit) { err_bol = 0; continue; }
        fputc(*s, stderr); err_bol = *s == '\n'; s++;
    }
}
void vv_veprintf(const char *fmt, va_list ap) {
    vv_uart_flush();                             /* keep stdout/stderr interleaving in order */
    char sb[1024], *b = sb; va_list aq; va_copy(aq, ap);
    int n = vsnprintf(sb, sizeof sb, fmt, ap);
    if (n < 0) { va_end(aq); return; }
    if ((size_t)n >= sizeof sb && (b = malloc(n + 1))) vsnprintf(b, n + 1, fmt, aq);
    else if (!b) b = sb;
    va_end(aq);
    if (vv_color_err) err_colored(b);
    else { fputs(b, stderr); if (n) err_bol = b[strlen(b) - 1] == '\n'; }
    if (b != sb) free(b);
}
void vv_eprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vv_veprintf(fmt, ap); va_end(ap); }

const char *vv_creg(const char *name, char *buf, unsigned n) {
    if (!vv_color_err) return name;
    snprintf(buf, n, "\033[1;36m%s" RESET, name);
    return buf;
}
