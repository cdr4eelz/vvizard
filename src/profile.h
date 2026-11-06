/* profile.h - per-slot counters for `make PROFILE=1` (docs/PROFILING-PLAN.md).
 * With VV_PROFILE unset every macro is empty, so the default build does not pay for them.
 * Counts are always on in a PROFILE=1 build. Timers run only when --profile is passed:
 * an instruction is ~100 ns, so a timer pair per instruction would dominate the result. */
#ifndef VV_PROFILE_H
#define VV_PROFILE_H
#include <stdint.h>

enum {
    VV_PS_EMU = 0,       /* wall around the CPU run (uc_emu_start or the fast2/rtl runner) */
    VV_PS_HOOK,          /* hook_code calls */
    VV_PS_QUIRK_PRE,
    VV_PS_QUIRK_POST,
    VV_PS_HW_TICK,
    VV_PS_IRQ,
    VV_PS_MMIO_R,
    VV_PS_MMIO_W,
    VV_PS_TLB,
    VV_PS_UNMAPPED,
    VV_PS_BIOS_W,
    VV_PS_DC_LOAD,
    VV_PS_DC_STORE,
    VV_PS_FAST2,         /* instructions the fast2/rtl backend actually executed */
    VV_PS_N
};

void vv_prof_count(int slot);
void vv_prof_add_ns(int slot, uint64_t ns);
void vv_prof_emu_begin(void);
void vv_prof_emu_end(void);
void vv_prof_enable_timing(void);   /* --profile */
void vv_prof_report(void);
extern int vv_prof_timing;

#ifdef VV_PROFILE
#define VV_PROF_COUNT(s) vv_prof_count(s)
#else
#define VV_PROF_COUNT(s) ((void)0)
#endif
#endif
