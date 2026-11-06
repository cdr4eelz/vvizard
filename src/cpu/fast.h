/* Fast L0 predecoded basic-block MIPS interpreter (plan option C).
 * Default via --cpu=fast; Unicorn is the explicit reference (--cpu=unicorn). */
#ifndef VV_CPU_FAST_H
#define VV_CPU_FAST_H
#include <stdint.h>
#include <unicorn/unicorn.h>

enum { VV_CPU_UNICORN = 0, VV_CPU_FAST = 1 };

typedef struct vv_fast_cpu {
    uint32_t gpr[32];
    uint32_t pc, hi, lo;
    int      load_rt;       /* load-delay: next insn sees OLD rt; -1 = none */
    uint32_t load_old;
    int      in_delay;      /* delay slot pending */
    uint32_t delay_npc;
    uint32_t last_pc, last2_pc, last3_pc;
    int      stop_break;    /* hit `break` */
    int      stop_until;
} vv_fast_cpu;

typedef struct vv_fast_env {
    uc_engine *uc;
    uint64_t  *insn_count;
    uint64_t  *hook_calls;
    uint64_t   max_insns;
    int       *stopped_max;
    int       *stop_req;
    int       *stopped_wall;
    uint64_t   max_wall_ns;
    uint64_t   run_t0;
    int        have_until;
    uint32_t   until_addr;
    int        opt_irq;
    int        opt_trace;
    int       *trace_left;
    int        guest_big;   /* 1 = big-endian guest (product default) */
    /* MMIO (owned by main.c; still need uc for PF/GPU side effects) */
    uint64_t (*mmio_read)(uc_engine *uc, uint64_t off, unsigned size, void *u);
    void     (*mmio_write)(uc_engine *uc, uint64_t off, unsigned size, uint64_t val, void *u);
    void     (*note_ddr_exec)(uint32_t pc);
    void     (*run_due_events)(uc_engine *uc, int frame_kind);
    int      nevents;
} vv_fast_env;

/* Run until stop. Returns 0 on clean stop, 1 on fault/unsupported. */
int  vv_fast_run(vv_fast_env *env, vv_fast_cpu *cpu, uint32_t entry);
void vv_fast_sync_to_uc(uc_engine *uc, const vv_fast_cpu *cpu);
/* While vv_fast_run is active: non-NULL so display publish can snapshot GPRs into UC
 * at frame rate (not every insn). UI must not read UC regs live from another thread. */
extern const vv_fast_cpu *vv_fast_live_cpu;
void vv_fast_publish_prepare(uc_engine *uc); /* no-op if no live fast CPU */
void vv_fast_invalidate(uint32_t addr, uint32_t nbytes);
void vv_fast_flush(void);

#endif
