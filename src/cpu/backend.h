/* backend.h - which CPU runs the guest.  Unicorn stays the default. */
#ifndef VV_BACKEND_H
#define VV_BACKEND_H
#include <stdint.h>
#include <unicorn/unicorn.h>

typedef enum { VV_CPU_UNICORN = 0, VV_CPU_FAST = 1, VV_CPU_RTL = 2 } vv_cpu_kind_t;
extern vv_cpu_kind_t vv_cpu_kind;

/* Optional code-cache drop for the fast backend.  NULL on the Unicorn path.
 * hw.c calls this next to uc_ctl_remove_cache when the GPU wrote DDR the guest has fetched. */
extern void (*vv_code_invalidate)(uint32_t lo, uint32_t hi);

/* Dispatch --cpu=fast or --cpu=rtl.  `until` is used only when has_until is set. */
uc_err vv_cpu_run(uc_engine *uc, uint32_t entry, uint32_t until, int has_until);

/* Register view for dump / --expect / --trace-regs.  Unicorn reads the engine; fast/rtl read theirs. */
uint32_t vv_view_gpr(uc_engine *uc, int r);
uint32_t vv_view_pc(uc_engine *uc);
uint32_t vv_view_hi(uc_engine *uc);
uint32_t vv_view_lo(uc_engine *uc);
uint32_t vv_view_status(uc_engine *uc);
#endif
