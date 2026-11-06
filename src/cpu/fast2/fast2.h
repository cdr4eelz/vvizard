/* fast2.h - predecoded basic-block interpreter (--cpu=fast2).  No cache model lives here. */
#ifndef VV_FAST2_H
#define VV_FAST2_H
#include <stdint.h>
#include <unicorn/unicorn.h>

uint32_t vv_fast2_gpr(unsigned r);
void     vv_fast2_set_gpr(unsigned r, uint32_t v);
uint32_t vv_fast2_c0(void);
void     vv_fast2_set_c0(uint32_t v);
uint32_t vv_fast2_pc(void);

/* Fetch the word the fast2 CPU would execute, without reporting a quirk or touching a cache.
 * *dead is 1 when hardware would fetch 0 (undecoded or MMIO). */
uint32_t vv_fast2_fetch_raw(uint32_t pc, int *dead);
/* If pc is a load or store, write the address (base+imm, no load-use substitution) and return 1. */
int vv_fast2_peek_mem(uint32_t pc, uint32_t *addr, int *store);

uc_err vv_fast2_run(uc_engine *uc, uint32_t entry, uint32_t until, int has_until);

/* 1 when an aligned fetch at pc would miss with unmapped-fetch off.  Unicorn fails that fetch
 * before the instruction is counted.  Misaligned PCs are a counted AdEL instead. */
int vv_fast2_fetch_aborts(uint32_t pc);
/* 1 when the last stop was that kind of memory fault (no CPU-exception line). */
int vv_fast2_unhandled(void);

/* Execute the instruction already counted at *pc.  Updates *pc.  Returns 0 to continue, 1 to stop
 * (halt, break, or another exception; the exception line is already printed). */
int vv_fast2_step(uc_engine *uc, uint32_t *pc);
void vv_fast2_init(uc_engine *uc, uint32_t entry);
void vv_fast2_cancel_lu(void);
#endif /* VV_FAST2_H */
