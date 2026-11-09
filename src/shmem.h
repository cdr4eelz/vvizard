/* shmem.h - decoder for the proj6/proj7 "shared memory" structure (struct SM_DATA) at SM_BASE_ADDR
 * (0x50000000, D-scratch), shared between a projN app and its ISR.  vvizard only reads it, for the
 * window status strip and --dump-sm; the guest owns it.
 *
 * Layouts (offsets in bytes; u16/i16 fields are big-endian halfwords like everything else):
 *   proj7  software/proj_app7/shared7.h (and shared7.s.inc): K_MAGIC_VERSION 0xFEDBEEF4, K_MAJOR_VERSION 7
 *   proj6  software/proj_app6/shared6.h (and shared6.s.inc): K_MAGIC_VERSION 0xFEDBEEF2 (no majorvers field)
 * Detection: magic1 (offset 0) must be 0xFEDBEEFn.  The proj7 layout is chosen when magic1 is
 * 0xFEDBEEF3 or higher, or when magic2 (proj7 offset 0x13C) equals magic1; otherwise 0xFEDBEEF2 is
 * the proj6 layout.  Older proj7 stamps (0xFEDBEEF3, or a proj6-valued stamp on a proj7 layout) still
 * decode as proj7; a stamp that disagrees with shared7.h is reported instead of mis-reading fields.
 * VV_SM7_* (and the flags) are checked against the vendored vendor/cs150/software/proj_app7/shared7.h at
 * compile time (src/cs150_host/cs150_check.c); VV_SM6_* are not (shared6.h is not vendored).  Guest memory is
 * big-endian: read through the vv_sm_reader callback, never by overlaying struct SM_DATA. */
#ifndef VV_SHMEM_H
#define VV_SHMEM_H
#include <stdint.h>
#include <stddef.h>

#define VV_SM_MAGIC_BASE   0xFEDBEEF0u   /* K_MAGIC_VERSION = 0xFEDBEEF0 | n */
#define VV_SM6_MAGIC       0xFEDBEEF2u   /* shared6.h K_MAGIC_VERSION */
#define VV_SM7_LAYOUT_MIN  0xFEDBEEF3u   /* first proj7 stamp; layout detection floor */
#define VV_SM7_MAGIC       0xFEDBEEF4u   /* shared7.h K_MAGIC_VERSION (current) */
#define VV_SM7_MAJOR       7u            /* shared7.h K_MAJOR_VERSION */
#define VV_SM_BUFSZ        0x0100u       /* K_SHBUF_SIZEB (both) */

/* shared7.h SMO_* */
#define VV_SM7_magic1        0x0000u
#define VV_SM7_majorvers     0x0004u
#define VV_SM7_stash0        0x0008u
#define VV_SM7_state         0x0018u   /* u16 */
#define VV_SM7_flags         0x001Au   /* u16 */
#define VV_SM7_RTC_count     0x001Cu
#define VV_SM7_seconds       0x0020u
#define VV_SM7_clock         0x0024u
#define VV_SM7_player_y      0x0028u   /* i16 */
#define VV_SM7_player_x      0x002Au   /* i16 */
#define VV_SM7_max_bouncers 0x002Cu   /* u16 */
#define VV_SM7_num_bouncers 0x002Eu   /* u16 */
#define VV_SM7_buff_size     0x0030u
#define VV_SM7_buff_head     0x0034u
#define VV_SM7_buff_tail     0x0038u
#define VV_SM7_buff_data     0x003Cu
#define VV_SM7_magic2        (VV_SM7_buff_data + VV_SM_BUFSZ)   /* 0x13C */
#define VV_SM7_PAST_END      (VV_SM7_magic2 + 4)                /* 0x140 */
/* shared6.h SMO_* */
#define VV_SM6_magic         0x0000u
#define VV_SM6_stash0        0x0004u
#define VV_SM6_state         0x0014u   /* u16 */
#define VV_SM6_flags         0x0016u   /* u16 */
#define VV_SM6_RTC_count     0x0018u
#define VV_SM6_seconds       0x001Cu
#define VV_SM6_clock         0x0020u
#define VV_SM6_player_y      0x0024u   /* i16 */
#define VV_SM6_player_x      0x0026u   /* i16 */
#define VV_SM6_buff_size     0x0028u
#define VV_SM6_buff_head     0x002Cu
#define VV_SM6_buff_tail     0x0030u
#define VV_SM6_buff_data     0x0034u
#define VV_SM6_PAST_END      (VV_SM6_buff_data + VV_SM_BUFSZ)   /* 0x134 */
/* SMM_flags_* (bit 2, quit, is proj7 only) */
#define VV_SM_FLAG_PAUSE     0x0001u
#define VV_SM_FLAG_NOTIMER   0x0002u
#define VV_SM_FLAG_QUIT      0x0004u

typedef struct {
    int      layout;               /* 0: none found, 6: shared6.h, 7: shared7.h */
    uint32_t magic1, majorvers, magic2;   /* majorvers/magic2: proj7 only */
    uint16_t state, flags;
    uint32_t rtc_count, seconds, clock;   /* clock: BCD mm:ss in the low 16 bits */
    int16_t  player_y, player_x;
    uint16_t max_bouncers, num_bouncers;  /* proj7 only */
    uint32_t buff_size, buff_head, buff_tail;
    int      nwarn;
    char     warn[4][224];         /* consistency problems (stamp, magic2, buffer) */
} vv_sm;

/* Reads n (1, 2 or 4) bytes of guest memory at addr as a guest-order value (0 when unreadable). */
typedef uint32_t (*vv_sm_reader)(void *ctx, uint32_t addr, unsigned n);
/* Decode the structure at base; returns s->layout. */
int  vv_sm_read(vv_sm_reader rd, void *ctx, uint32_t base, vv_sm *s);
/* Short form for the status strip, e.g. "proj7 graphics running  clock 01:23  player 400,300  bncrs 0/127". */
void vv_sm_status(const vv_sm *s, uint32_t base, char *out, size_t n);
/* Multi-line report to stderr ("[sm] ..."), for --dump-sm. */
void vv_sm_dump(const vv_sm *s, uint32_t base);
/* Print only the consistency warnings ("[sm] WARNING ..."), if any. */
void vv_sm_warn(const vv_sm *s, uint32_t base);
#endif
