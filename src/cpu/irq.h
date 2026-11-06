/* irq.h - COP0150 / CPUMIPS.v DO_ISR, shared by Unicorn and the fast2 backend. */
#ifndef VV_IRQ_H
#define VV_IRQ_H
#include <stdint.h>
#include <unicorn/unicorn.h>

extern uint64_t vv_irq_count;

/* One instruction boundary.  uc == NULL reads the fast2 register file; otherwise Unicorn's.
 * Returns 1 and writes *new_pc = 0xC0000180 when the interrupt is taken (the insn at pc does not run). */
int vv_irq_step(uc_engine *uc, uint32_t pc, uint32_t *new_pc);
#endif
