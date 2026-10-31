/* dcache.c - see dcache.h */
#include "dcache.h"
#include "memmap.h"
#include <string.h>

static struct {
    uint32_t tag;          /* addr[27:13] */
    uint8_t  valid;
    uint8_t  stale;        /* bit w: word w differs from DDR (GPU wrote DDR after the fill) */
    uint32_t data[8];      /* the cached value of each stale word */
} line[VV_DC_LINES];
static uint64_t stale_words;

static unsigned idx(uint32_t a) { return (a >> 5) & (VV_DC_LINES - 1); }
static uint32_t tag(uint32_t a) { return (a >> 13) & 0x7FFFu; }
static int hit(uint32_t a) { unsigned i = idx(a); return line[i].valid && line[i].tag == tag(a); }

int vv_dc_load(uint32_t a, uint32_t *old) {
    unsigned i = idx(a), w = (a >> 2) & 7;
    if (!hit(a)) { line[i].valid = 1; line[i].tag = tag(a); line[i].stale = 0; return 0; }   /* miss: fill from DDR */
    if (!(line[i].stale >> w & 1)) return 0;
    *old = line[i].data[w];
    return 1;
}
void vv_dc_store(uint32_t a, unsigned size, uint32_t v) {
    unsigned i = idx(a), w = (a >> 2) & 7;
    if (!hit(a) || !(line[i].stale >> w & 1)) return;      /* miss (no allocate) or coherent word */
    if (size >= 4) { line[i].stale &= (uint8_t)~(1u << w); return; }   /* the whole word is new in both */
    unsigned sh = 8 * (4 - size - (a & 3));                 /* big-endian lane */
    uint32_t m = ((1u << (8 * size)) - 1) << sh;
    line[i].data[w] = (line[i].data[w] & ~m) | ((v << sh) & m);   /* still differs in the other bytes */
}
void vv_dc_dma_write(uc_engine *uc, uint32_t a, size_t len) {
    (void)uc; if (!len) return;
    uint32_t end = a + (uint32_t)len;                       /* exclusive */
    for (uint32_t la = a & ~(VV_DC_LINE - 1); la < end; la += VV_DC_LINE) {
        if (!hit(la)) continue;
        unsigned i = idx(la);
        for (unsigned w = 0; w < 8; w++) {
            uint32_t wa = la + 4 * w;
            if (wa + 4 <= a || wa >= end || (line[i].stale >> w & 1)) continue;
            int ok = 1; uint32_t v = vv_ram_be32(wa, &ok);     /* (before the GPU's uc_mem_write) */
            if (!ok) continue;
            line[i].data[w] = v;
            line[i].stale |= (uint8_t)(1u << w); stale_words++;
        }
    }
}
uint64_t vv_dc_stale_words(void) { return stale_words; }
