/* cs150_check.c - compile-time binding of vvizard's VV_* constants to the vendored cs150 headers.
 *
 * Built by every `make` (build/cs150_check.o, a prerequisite of vvizard but NOT linked into it).
 * Each _Static_assert fails the build if src/memmap.h, src/hw.h or src/shmem.h drift from the
 * snapshot in vendor/cs150/ (mmio_intr_cop0.h, shared7.h).  Comparing that snapshot with the live
 * cs150 tree is the Makefile's vendor-cmp / check-vendor job (docs/VENDORED-HEADERS-PLAN.md).
 *
 * Only constants and the struct SM_DATA layout are used: guest memory is big-endian and vvizard
 * reads it field by field (src/shmem.c); nothing here dereferences SM_BASE or runs the MIPS asm
 * macros (HALT, ISR_STATUS, ISR_COMPARE).  shared6.h (VV_SM6_*) is not vendored. */
#include "types_guard.h"
#include "mmio_intr_cop0.h"
#include "shared7.h"
#include "memmap.h"
#include "hw.h"
#include "shmem.h"

#undef HALT
#undef ISR_STATUS
#undef ISR_COMPARE
#undef SM_BASE
#define HALT(v)          VV_DO_NOT_CALL_MIPS_ASM_HALT
#define ISR_STATUS(k, s) VV_DO_NOT_CALL_MIPS_ASM_ISR_STATUS
#define ISR_COMPARE(c)   VV_DO_NOT_CALL_MIPS_ASM_ISR_COMPARE

#define VV_EQ(a, b) _Static_assert((unsigned long)(a) == (unsigned long)(b), #a " == " #b)

/* MMIO: src/memmap.h VV_OW_*  vs  mmio_intr_cop0.h OW_* */
VV_EQ(VV_MMIO_BASE,     MMIO_BASE);
VV_EQ(VV_OW_UATX_READY, OW_UATX_READY);
VV_EQ(VV_OW_UARX_VALID, OW_UARX_VALID);
VV_EQ(VV_OW_UATX_DATA,  OW_UATX_DATA);
VV_EQ(VV_OW_UARX_DATA,  OW_UARX_DATA);
VV_EQ(VV_OW_CNT_CYCLE,  OW_CNT_CYCLE);
VV_EQ(VV_OW_CNT_INST,   OW_CNT_INST);
VV_EQ(VV_OW_CNT_RESET,  OW_CNT_RESET);
VV_EQ(VV_OW_PF_FRAME,   OW_PF_FRAME);
VV_EQ(VV_OW_GP_FRAME,   OW_GP_FRAME);
VV_EQ(VV_OW_GP_GCODE,   OW_GP_GCODE);
VV_EQ(VV_OW_GP_STATE,   OW_GP_STATE);
VV_EQ(VV_OW_RANDOM32,   OW_RANDOM32);
VV_EQ(VV_OW_HALTVAL,    OW_HALTVAL);
VV_EQ(VV_SM_BASE_ADDR,  SM_BASE_ADDR);

/* Cause/Status: src/hw.h VV_IP_*  vs  IM_*  (names differ: GP/PF vs GPU/FRAME) */
VV_EQ(VV_IP_UARX,  IM_UARX);
VV_EQ(VV_IP_UATX,  IM_UATX);
VV_EQ(VV_IP_GP,    IM_GPU);
VV_EQ(VV_IP_PF,    IM_FRAME);
VV_EQ(VV_IP_RTC,   IM_RTC);
VV_EQ(VV_IP_TIMER, IM_TIMER);
VV_EQ(VV_IP_ALL,   IM_POSSIBLE);
VV_EQ(VV_CPU_HZ,   K_CPU_HZ);

/* src/shmem.h proj7 constants and offsets  vs  shared7.h */
VV_EQ(VV_SM7_MAGIC,         K_MAGIC_VERSION);
VV_EQ(VV_SM7_MAJOR,         K_MAJOR_VERSION);
VV_EQ(VV_SM_BUFSZ,          K_SHBUF_SIZEB);
VV_EQ(VV_SM7_magic1,        SMO_magic1);
VV_EQ(VV_SM7_majorvers,     SMO_majorvers);
VV_EQ(VV_SM7_stash0,        SMO_stash0);
VV_EQ(VV_SM7_state,         SMO_state);
VV_EQ(VV_SM7_flags,         SMO_flags);
VV_EQ(VV_SM7_RTC_count,     SMO_RTC_count);
VV_EQ(VV_SM7_seconds,       SMO_seconds);
VV_EQ(VV_SM7_clock,         SMO_clock);
VV_EQ(VV_SM7_player_y,      SMO_player_y);
VV_EQ(VV_SM7_player_x,      SMO_player_x);
VV_EQ(VV_SM7_max_bouncers, SMO_max_bouncers);
VV_EQ(VV_SM7_num_bouncers, SMO_num_bouncers);
VV_EQ(VV_SM7_buff_size,     SMO_buff_size);
VV_EQ(VV_SM7_buff_head,     SMO_buff_head);
VV_EQ(VV_SM7_buff_tail,     SMO_buff_tail);
VV_EQ(VV_SM7_buff_data,     SMO_buff_data);
VV_EQ(VV_SM7_magic2,        SMO_magic2);
VV_EQ(VV_SM7_PAST_END,      SMO_PAST_END);
VV_EQ(VV_SM_FLAG_PAUSE,     SMM_flags_pause);
VV_EQ(VV_SM_FLAG_NOTIMER,   SMM_flags_notimer);
VV_EQ(VV_SM_FLAG_QUIT,      SMM_flags_quit);

/* shared7.h's own consistency: struct SM_DATA vs its SMO_* offsets (shared7.c SM_CHECK_OFFSET, at
 * compile time).  SM_DATA has no pointer members, so host offsets equal the MIPS o32 ones. */
#define VV_OFF(member, expected) \
    _Static_assert(__builtin_offsetof(struct SM_DATA, member) == (expected), "SM_DATA." #member " offset")
VV_OFF(magic1, SMO_magic1);
VV_OFF(majorvers, SMO_majorvers);
VV_OFF(stash0, SMO_stash0);
VV_OFF(stash1, SMO_stash1);
VV_OFF(stash2, SMO_stash2);
VV_OFF(stash3, SMO_stash3);
VV_OFF(state, SMO_state);
VV_OFF(flags, SMO_flags);
VV_OFF(RTC_count, SMO_RTC_count);
VV_OFF(seconds, SMO_seconds);
VV_OFF(clock, SMO_clock);
VV_OFF(player_y, SMO_player_y);
VV_OFF(player_x, SMO_player_x);
VV_OFF(max_bouncers, SMO_max_bouncers);
VV_OFF(num_bouncers, SMO_num_bouncers);
VV_OFF(buff_size, SMO_buff_size);
VV_OFF(buff_head, SMO_buff_head);
VV_OFF(buff_tail, SMO_buff_tail);
VV_OFF(buff_data, SMO_buff_data);
VV_OFF(magic2, SMO_magic2);
_Static_assert(sizeof(((struct SM_DATA *)0)->buff_data) == K_SHBUF_SIZEB, "SM_DATA.buff_data size");
_Static_assert(sizeof(struct SM_DATA) == SMO_PAST_END, "sizeof(struct SM_DATA) == SMO_PAST_END");
_Static_assert(sizeof(struct SM_DATA) == VV_SM7_PAST_END, "sizeof(struct SM_DATA) == VV_SM7_PAST_END");
_Static_assert(sizeof(uint32_t) == 4 && sizeof(uint16_t) == 2 && sizeof(int16_t) == 2 && sizeof(int8_t) == 1,
               "SM_DATA field widths");

/* ISO C wants at least one declaration in a translation unit. */
typedef int vv_cs150_check_tu;
