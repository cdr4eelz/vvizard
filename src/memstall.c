/* memstall.c - see memstall.h */
#include "memstall.h"
#include "memmap.h"
#include "term.h"
#include <string.h>

int vv_ddr_stalls;

typedef struct { uint32_t tag; uint8_t valid; } ms_line;
static ms_line ic[VV_MS_LINES], dc[VV_MS_LINES];
static uint64_t stall_sum, imiss, dmiss, dwrite, host_cycles;

static unsigned idx(uint32_t a) { return (a >> 5) & (VV_MS_LINES - 1); }
static uint32_t tag_of(uint32_t a) { return (a >> 13) & 0x7FFFu; }

static int is_ddr_fetch(uint32_t a) {
    unsigned n = a >> 28;
    return n == 0x1 || n == 0x2 || n == 0x3;
}
static int is_ddr_data(uint32_t a) {
    unsigned n = a >> 28;
    return n == 0x1 || n == 0x3;
}

void vv_ms_reset(void) {
    memset(ic, 0, sizeof ic);
    memset(dc, 0, sizeof dc);
    stall_sum = imiss = dmiss = dwrite = host_cycles = 0;
}

static unsigned fill(ms_line *c, uint32_t a) {
    unsigned i = idx(a);
    if (c[i].valid && c[i].tag == tag_of(a)) return 0;          /* hit: no extra stall */
    c[i].valid = 1;
    c[i].tag = tag_of(a);
    return VV_MS_MISS_CYCLES;
}

unsigned vv_ms_ifetch(uint32_t pc) {
    if (!vv_ddr_stalls) return 0;
    uint32_t c = vv_canon_inl(pc);
    if (c == 0xFFFFFFFFu || !is_ddr_fetch(c)) return 0;
    unsigned s = fill(ic, c);
    if (s) { imiss++; stall_sum += s; }
    return s;
}

unsigned vv_ms_load(uint32_t canon) {
    if (!vv_ddr_stalls) return 0;
    if (!is_ddr_data(canon)) return 0;
    unsigned s = fill(dc, canon);
    if (s) { dmiss++; stall_sum += s; }
    return s;
}

unsigned vv_ms_store(uint32_t canon) {
    if (!vv_ddr_stalls) return 0;
    if (!is_ddr_data(canon)) return 0;
    /* Write-through, no write-allocate: always pay write latency; update line only on hit. */
    unsigned i = idx(canon);
    if (dc[i].valid && dc[i].tag == tag_of(canon)) {
        /* hit: data would update; tags unchanged */
    }
    /* miss: no allocate (Cache.v) */
    dwrite++;
    stall_sum += VV_MS_WRITE_CYCLES;
    return VV_MS_WRITE_CYCLES;
}

unsigned vv_ms_dside_uc(uc_engine *uc, uint32_t pc) {
    if (!vv_ddr_stalls) return 0;
    uint8_t b[4];
    if (uc_mem_read(uc, pc, b, 4)) return 0;
    uint32_t insn = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    unsigned op = insn >> 26;
    int is_store = 0, is_load = 0;
    switch (op) {
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: /* lb lh lw lbu lhu */
    case 0x22: case 0x26: /* lwl lwr */
        is_load = 1; break;
    case 0x28: case 0x29: case 0x2b: /* sb sh sw */
    case 0x2a: case 0x2e: /* swl swr */
        is_store = 1; break;
    default: return 0;
    }
    unsigned rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
    (void)rt;
    int32_t imm = (int16_t)(insn & 0xFFFFu);
    uint32_t rsv = 0;
    if (rs) uc_reg_read(uc, UC_MIPS_REG_0 + (int)rs, &rsv);
    uint32_t va = rsv + (uint32_t)imm;
    uint32_t c = vv_canon_inl(va);
    if (c == 0xFFFFFFFFu) return 0;
    if (is_load) return vv_ms_load(c);
    if (is_store) return vv_ms_store(c);
    return 0;
}

uint64_t vv_ms_stall_cycles(void) { return stall_sum; }
uint64_t vv_ms_cycles(void) { return host_cycles; }
uint64_t vv_ms_advance(unsigned stall) {
    host_cycles += 1u + (uint64_t)stall;
    return host_cycles;
}
void vv_ms_add_cycles(unsigned stall) {
    host_cycles += stall;
}
uint64_t vv_ms_imiss(void) { return imiss; }
uint64_t vv_ms_dmiss(void) { return dmiss; }
uint64_t vv_ms_dwrite(void) { return dwrite; }

void vv_ms_summary(void) {
    if (!vv_ddr_stalls) return;
    vv_eprintf("[ddr] stalls on (miss=%u write=%u): I$ misses %llu D$ misses %llu D$ writes %llu; "
               "stall cycles %llu, host cycles %llu (approx; caches gated on --ddr-stalls)\n",
               VV_MS_MISS_CYCLES, VV_MS_WRITE_CYCLES,
               (unsigned long long)imiss, (unsigned long long)dmiss, (unsigned long long)dwrite,
               (unsigned long long)stall_sum, (unsigned long long)host_cycles);
}
