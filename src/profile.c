/* profile.c - counters behind src/profile.h.  Linked always; the default build never calls in. */
#include "profile.h"
#include "term.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

int vv_prof_timing;

static uint64_t counts[VV_PS_N];
static uint64_t nanos[VV_PS_N];
static uint64_t emu_t0, emu_ns;
static const char *const name[VV_PS_N] = {
    "emu", "hook_code", "quirk_pre", "quirk_post", "hw_tick", "irq",
    "mmio_r", "mmio_w", "tlb_fill", "unmapped", "bios_w", "dc_load", "dc_store", "fast2_insn"
};

static uint64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void vv_prof_count(int slot) { if ((unsigned)slot < VV_PS_N) counts[slot]++; }
void vv_prof_add_ns(int slot, uint64_t ns) { if ((unsigned)slot < VV_PS_N) nanos[slot] += ns; }
void vv_prof_enable_timing(void) { vv_prof_timing = 1; }
void vv_prof_emu_begin(void) { emu_t0 = mono_ns(); }
void vv_prof_emu_end(void) { if (emu_t0) emu_ns += mono_ns() - emu_t0; }

void vv_prof_report(void) {
#ifndef VV_PROFILE
    (void)name;
    return;
#else
    /* One calibration: empty timer-pair cost, so a reader can see the probe overhead. */
    uint64_t c0 = mono_ns();
    for (int i = 0; i < 1000000; i++) { uint64_t a = mono_ns(); (void)a; }
    uint64_t per = (mono_ns() - c0) / 1000000ull;
    vv_eprintf("[profile] emu wall %.3f ms; clock_gettime pair ~%llu ns (1e6-sample calibration)\n",
               emu_ns / 1e6, (unsigned long long)per);
    for (int i = 0; i < VV_PS_N; i++) if (counts[i] || nanos[i])
        vv_eprintf("[profile] %-12s count %llu%s\n", name[i], (unsigned long long)counts[i],
                   nanos[i] ? "" : "");
    if (nanos[VV_PS_EMU] || emu_ns)
        vv_eprintf("[profile] guest rate uses insn_count / emu wall (printed by the caller when it has both)\n");
    uint64_t hook = counts[VV_PS_HOOK] ? counts[VV_PS_HOOK] : 1;
    vv_eprintf("[profile] hook shares: quirk_pre %llu%% quirk_post %llu%% hw_tick %llu%% irq %llu%% (of hook_code)\n",
               (unsigned long long)(counts[VV_PS_QUIRK_PRE] * 100 / hook),
               (unsigned long long)(counts[VV_PS_QUIRK_POST] * 100 / hook),
               (unsigned long long)(counts[VV_PS_HW_TICK] * 100 / hook),
               (unsigned long long)(counts[VV_PS_IRQ] * 100 / hook));
#endif
}
