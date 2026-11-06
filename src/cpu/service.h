/* service.h - main.c services the fast/rtl backends call.  The Unicorn hook uses the same
 * enter/leave split, so device time, max-insns and tracing stay on one path. */
#ifndef VV_SERVICE_H
#define VV_SERVICE_H
#include <stdint.h>
#include <unicorn/unicorn.h>

/* Count this instruction and tick devices at cycle `tick_at` (L0: the new insn_count).
 * Returns 1 if the insn must not run (stop_req, max-insns, a halt raised by the tick). */
int vv_svc_enter_at(uc_engine *uc, uint32_t pc, uint64_t tick_at);
/* Periodic 4096-insn service and --trace.  Returns 1 if the insn must not run (max-seconds, window). */
int vv_svc_leave(uc_engine *uc, uint32_t pc);
/* Count a quiet instruction with no device tick (the runner commits time with vv_hw_set_now).
 * Returns 1 if it must not run; the instruction is then not counted. */
int vv_svc_quiet(uint32_t pc);
/* How many EXTRA instructions may run without a tick after the one just entered.
 * 0 when an IRQ can be taken, --trace is on, or a device/script/service event is due. */
uint64_t vv_svc_quiet_budget(void);
int vv_irqs_on(void);
int vv_guest_be(void);
/* stop_req: the current instruction is the last one that may run. */
int vv_guest_stopping(void);
/* Last CPU exception number reported, or -1.  Set by vv_report_exception. */
int vv_guest_intr(void);
/* --rtl-cache (default on).  Only the FIDELITY=rtl runner reads this. */
extern int vv_rtl_cache;

uint64_t vv_mmio_read(uc_engine *uc, uint64_t off, unsigned size);
void vv_mmio_write(uc_engine *uc, uint64_t off, unsigned size, uint64_t val);
void vv_warn_bios_write(uint32_t addr, int size, uint64_t val);
/* Same line hook_intr prints.  pc is whatever the Unicorn hook observed (often 0). */
void vv_report_exception(uint32_t intno, uint32_t pc);
#endif
