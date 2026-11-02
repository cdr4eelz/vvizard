/* shmem.c - proj6/proj7 shared-memory (struct SM_DATA) decoder; see shmem.h. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "shmem.h"
#include "term.h"

static void warn(vv_sm *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void warn(vv_sm *s, const char *fmt, ...) {
    if (s->nwarn >= (int)(sizeof s->warn / sizeof s->warn[0])) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(s->warn[s->nwarn++], sizeof s->warn[0], fmt, ap);
    va_end(ap);
}

int vv_sm_read(vv_sm_reader rd, void *ctx, uint32_t base, vv_sm *s) {
    memset(s, 0, sizeof *s);
    s->magic1 = rd(ctx, base, 4);
    if ((s->magic1 & ~0xFu) != VV_SM_MAGIC_BASE || (s->magic1 & 0xFu) < 2) return s->layout = 0;
    uint32_t m2 = rd(ctx, base + VV_SM7_magic2, 4);
    if (s->magic1 >= VV_SM7_MAGIC || m2 == s->magic1) {
        s->layout = 7;
        s->majorvers = rd(ctx, base + VV_SM7_majorvers, 4);
        s->magic2 = m2;
        s->state = (uint16_t)rd(ctx, base + VV_SM7_state, 2);
        s->flags = (uint16_t)rd(ctx, base + VV_SM7_flags, 2);
        s->rtc_count = rd(ctx, base + VV_SM7_RTC_count, 4);
        s->seconds = rd(ctx, base + VV_SM7_seconds, 4);
        s->clock = rd(ctx, base + VV_SM7_clock, 4);
        s->player_y = (int16_t)rd(ctx, base + VV_SM7_player_y, 2);
        s->player_x = (int16_t)rd(ctx, base + VV_SM7_player_x, 2);
        s->max_opponents = (uint16_t)rd(ctx, base + VV_SM7_max_opponents, 2);
        s->num_opponents = (uint16_t)rd(ctx, base + VV_SM7_num_opponents, 2);
        s->buff_size = rd(ctx, base + VV_SM7_buff_size, 4);
        s->buff_head = rd(ctx, base + VV_SM7_buff_head, 4);
        s->buff_tail = rd(ctx, base + VV_SM7_buff_tail, 4);
        if (s->magic1 != VV_SM7_MAGIC || s->majorvers != VV_SM7_MAJOR) {
            if (s->magic1 == VV_SM6_MAGIC && s->majorvers == 6)
                warn(s, "proj7 layout stamped 0x%08x / majorvers %u (proj6 values), shared7.h says 0x%08x / %u: "
                        "stale K_MAGIC_VERSION/K_MAJOR_VERSION (shared7.s.inc?)", s->magic1, s->majorvers, VV_SM7_MAGIC, VV_SM7_MAJOR);
            else
                warn(s, "stamp magic1 0x%08x / majorvers %u, shared7.h says 0x%08x / %u", s->magic1, s->majorvers,
                     VV_SM7_MAGIC, VV_SM7_MAJOR);
        }
        if (s->magic2 != s->magic1)
            warn(s, "magic2 (+0x%03x) 0x%08x != magic1 0x%08x: structure size/offsets disagree", VV_SM7_magic2,
                 s->magic2, s->magic1);
        if ((s->magic1 & 0xFu) > 3)
            warn(s, "magic 0x%08x is newer than proj7 (decoded with the shared7.h layout)", s->magic1);
    } else {
        s->layout = 6;
        s->state = (uint16_t)rd(ctx, base + VV_SM6_state, 2);
        s->flags = (uint16_t)rd(ctx, base + VV_SM6_flags, 2);
        s->rtc_count = rd(ctx, base + VV_SM6_RTC_count, 4);
        s->seconds = rd(ctx, base + VV_SM6_seconds, 4);
        s->clock = rd(ctx, base + VV_SM6_clock, 4);
        s->player_y = (int16_t)rd(ctx, base + VV_SM6_player_y, 2);
        s->player_x = (int16_t)rd(ctx, base + VV_SM6_player_x, 2);
        s->buff_size = rd(ctx, base + VV_SM6_buff_size, 4);
        s->buff_head = rd(ctx, base + VV_SM6_buff_head, 4);
        s->buff_tail = rd(ctx, base + VV_SM6_buff_tail, 4);
    }
    if (s->buff_size != VV_SM_BUFSZ)
        warn(s, "buff_size %u, K_SHBUF_SIZEB is %u", s->buff_size, VV_SM_BUFSZ);
    if (s->buff_head >= VV_SM_BUFSZ || s->buff_tail >= VV_SM_BUFSZ)
        warn(s, "ring buffer head %u / tail %u outside 0..%u", s->buff_head, s->buff_tail, VV_SM_BUFSZ - 1);
    return s->layout;
}

void vv_sm_status(const vv_sm *s, uint32_t base, char *out, size_t n) {
    if (!s->layout) { snprintf(out, n, "graphics -  clock -  (no proj6/7 shared memory at 0x%08x)", base); return; }
    unsigned c = s->clock;
    int k = snprintf(out, n, "proj%d graphics %s  clock %X%X:%X%X%s  player %d,%d", s->layout,
                     s->flags & VV_SM_FLAG_PAUSE ? "PAUSED" : "running", c >> 12 & 15, c >> 8 & 15, c >> 4 & 15, c & 15,
                     s->flags & VV_SM_FLAG_NOTIMER ? " (uart off)" : "", s->player_x, s->player_y);
    if (k > 0 && (size_t)k < n && s->layout == 7)
        k += snprintf(out + k, n - k, "  opp %u/%u%s", s->num_opponents, s->max_opponents, s->flags & VV_SM_FLAG_QUIT ? "  QUIT" : "");
    if (k > 0 && (size_t)k < n && s->nwarn)
        snprintf(out + k, n - k, "  STAMP?");
}

void vv_sm_warn(const vv_sm *s, uint32_t base) {
    for (int i = 0; i < s->nwarn; i++) vv_eprintf("[sm] WARNING at 0x%08x: %s\n", base, s->warn[i]);
}

void vv_sm_dump(const vv_sm *s, uint32_t base) {
    if (!s->layout) {
        vv_eprintf("[sm] no proj6/proj7 shared memory at 0x%08x (magic 0x%08x)\n", base, s->magic1);
        return;
    }
    unsigned c = s->clock;
    vv_eprintf("[sm] proj%d shared memory at 0x%08x (shared%d.h layout, 0x%x bytes)\n", s->layout, base, s->layout,
               s->layout == 7 ? VV_SM7_PAST_END : VV_SM6_PAST_END);
    if (s->layout == 7)
        vv_eprintf("[sm]   magic1 0x%08x  majorvers %u  magic2 0x%08x\n", s->magic1, s->majorvers, s->magic2);
    else
        vv_eprintf("[sm]   magic 0x%08x\n", s->magic1);
    vv_eprintf("[sm]   state 0x%04x  flags 0x%04x (pause %u notimer %u%s)\n", s->state, s->flags,
               s->flags & VV_SM_FLAG_PAUSE ? 1u : 0u, s->flags & VV_SM_FLAG_NOTIMER ? 1u : 0u,
               s->layout == 7 ? (s->flags & VV_SM_FLAG_QUIT ? " quit 1" : " quit 0") : "");
    vv_eprintf("[sm]   RTC_count %u  seconds %u  clock 0x%04x (%X%X:%X%X)\n", s->rtc_count, s->seconds, c & 0xFFFFu,
               c >> 12 & 15, c >> 8 & 15, c >> 4 & 15, c & 15);
    if (s->layout == 7)
        vv_eprintf("[sm]   player x %d y %d  opponents %u/%u\n", s->player_x, s->player_y, s->num_opponents, s->max_opponents);
    else
        vv_eprintf("[sm]   player x %d y %d\n", s->player_x, s->player_y);
    vv_eprintf("[sm]   buff size %u head %u tail %u\n", s->buff_size, s->buff_head, s->buff_tail);
    vv_sm_warn(s, base);
}
