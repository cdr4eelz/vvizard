/* memstall.h - approximate DDR2 + I$/D$ stall timing (opt-in --ddr-stalls=R,W).
 * Caches and miss stalls are active ONLY when vv_ddr_stalls is set. Default off:
 * --cpu=fast stays the current 1-cycle-per-insn path with no per-insn tax.
 * Geometry matches cache.vh / Cache.v: 256 x 32B direct-mapped, write-through,
 * no write-allocate. Default stalls from UBU24 Icarus cache_tb (see
 * docs/DDR-CACHE-STALLS-PLAN.md): read/miss-fill = 16, write-through = 3,
 * hit = 0 extra. */
#ifndef VV_MEMSTALL_H
#define VV_MEMSTALL_H
#include <stdint.h>
#include <unicorn/unicorn.h>

extern int vv_ddr_stalls;                 /* --ddr-stalls (default 0 = off) */
extern unsigned vv_ms_read_stalls;        /* miss-fill cycles (default 16) */
extern unsigned vv_ms_write_stalls;       /* write-through cycles (default 3) */

#define VV_MS_LINES              256u
#define VV_MS_LINE               32u
#define VV_MS_READ_STALLS_DFLT   16u      /* cache_tb cold/conflict miss band low end */
#define VV_MS_WRITE_STALLS_DFLT  3u       /* cache_tb write-through */

void     vv_ms_reset(void);
void     vv_ms_enable(unsigned read_stalls, unsigned write_stalls); /* turn on + set costs */
/* Instruction fetch through I$ when PC is in DDR (nibbles 0x1/0x2/0x3). 0 if off. */
unsigned vv_ms_ifetch(uint32_t pc);
/* Data load/store through D$ for DDR data path (nibbles 0x1/0x3). 0 if off. */
unsigned vv_ms_load(uint32_t canon);
unsigned vv_ms_store(uint32_t canon);
/* Decode insn at pc; if it is a load/store to DDR data path, charge D$ (Unicorn path). */
unsigned vv_ms_dside_uc(uc_engine *uc, uint32_t pc);
uint64_t vv_ms_stall_cycles(void);        /* sum of stall penalties charged */
uint64_t vv_ms_cycles(void);              /* host cycle time when --ddr-stalls */
uint64_t vv_ms_advance(unsigned stall);   /* cycles += 1+stall; return new cycles */
void     vv_ms_add_cycles(unsigned stall); /* cycles += stall (D$ after exec) */
uint64_t vv_ms_imiss(void);
uint64_t vv_ms_dmiss(void);
uint64_t vv_ms_dwrite(void);
void     vv_ms_summary(void);             /* [ddr] line after [hw] when mode on */

#endif
