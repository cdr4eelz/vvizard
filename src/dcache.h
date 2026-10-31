/* dcache.h - D-cache coherence model for the dcache-stale quirk (base_mem/Cache.v, cache.vh).
 * The MIPS150 data cache is direct mapped, 256 lines x 32 bytes (8 KiB): offset addr[4:2], index
 * addr[12:5], tag addr[27:13] (cache.vh).  Write-through without write-allocate: a store always
 * goes to DDR and updates the line only on a hit; a read miss fills the whole line (Cache.v
 * IDLE->FETCH->READ1/2->CWRITEB).  There is no invalidate or flush, and the GPU (ScanLineRunner)
 * writes DDR directly, so a line the CPU has cached keeps the old data after a GPU write: the CPU
 * reads it stale until the line is evicted by another address with the same index.
 * Only the D path (nibbles 0x1 and 0x3) is modelled; the I-cache is not. */
#ifndef VV_DCACHE_H
#define VV_DCACHE_H
#include <stdint.h>
#include <stddef.h>
#include <unicorn/unicorn.h>
#define VV_DC_LINES 256u
#define VV_DC_LINE  32u
/* CPU load of the aligned word at canonical DDR address a (allocates on a miss).  Returns 1 and the
 * word the cache holds in *old when that word is stale (differs from DDR after a GPU write). */
int  vv_dc_load(uint32_t a, uint32_t *old);
/* CPU store of `size` bytes of v (low bits) at canonical a: keeps a stale cached word consistent. */
void vv_dc_store(uint32_t a, unsigned size, uint32_t v);
/* GPU (DMA) write of len bytes at canonical a, BEFORE the bytes change in DDR. */
void vv_dc_dma_write(uc_engine *uc, uint32_t a, size_t len);
uint64_t vv_dc_stale_words(void);           /* words made stale so far */
#endif
