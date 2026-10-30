/* prng.c - cycle-accurate-ish model of the MemMapIO PRNG (see prng.h).
 *
 * RTL (hardware/src/cpu/MemMapIO.v, hardware/src/base_util/PRNG32LFSR.v):
 *   PRNG32LFSR: on rst -> 0xACE12468; else if load_seed -> (seed==0 ? 1 : seed);
 *               else every posedge: s = s[0] ? (s>>1)^0x80200003 : s>>1.
 *   load_seed = isWrite && addra[5:0]==6'h20 (any byte mask; seed_val = full dina, which
 *               StageMW/ByteAccess4 has already shifted into the store's byte lane, zeros elsewhere).
 *   prng_out_r <= prng_out every clock; douta <= prng_out_r when isRead (registered output).
 * Let state(c) be prng_out during cycle c.  A read with ena in cycle c returns prng_out_r(c) =
 * state(c-1).  A store in cycle c makes state(c+1) = seed, so a read in the very next cycle
 * returns state(c) (stale, pre-seed), the read after that returns the seed, then seed advanced
 * by one step per elapsed cycle.  Back-to-back reads return consecutive LFSR states.
 *
 * Emulator approximation: "cycle" = vvizard's instruction count (each instruction = one cycle,
 * one memory access per instruction, no stalls/cache misses).  The PRNG is reset only at
 * emulator start (state(0) = 0xACE12468); CNT_RESET does not touch it (as in the RTL).
 * The LFSR is advanced lazily, only when the register is accessed. */
#include "prng.h"

static inline uint32_t step1(uint32_t s) { return (s & 1u) ? (s >> 1) ^ VV_PRNG_MASK : s >> 1; }

/* GF(2) 32x32 matrices as 32 column images; pw[k] = (one LFSR step)^(2^k). */
static uint32_t pw[64][32]; static int pw_ready;
static uint32_t apply(const uint32_t *m, uint32_t s) {
    uint32_t r = 0;
    for (int j = 0; s; j++, s >>= 1) if (s & 1u) r ^= m[j];
    return r;
}
static void init_pw(void) {
    for (int j = 0; j < 32; j++) pw[0][j] = step1(1u << j);   /* the step is linear over GF(2) */
    for (int k = 1; k < 64; k++)
        for (int j = 0; j < 32; j++) pw[k][j] = apply(pw[k - 1], pw[k - 1][j]);
    pw_ready = 1;
}
uint32_t vv_prng_advance(uint32_t s, uint64_t n) {
    if (n < 64) { while (n--) s = step1(s); return s; }
    if (!pw_ready) init_pw();
    for (int k = 0; n; k++, n >>= 1) if (n & 1u) s = apply(pw[k], s);
    return s;
}

static uint32_t st = VV_PRNG_RESET; static uint64_t st_at;          /* state(st_at) == st */
static uint32_t stale; static uint64_t stale_at = UINT64_MAX;      /* state(c) of the last store's cycle c */

static uint32_t state_at(uint64_t t) {
    if (t < st_at) return t == stale_at ? stale : st;   /* only t == store cycle can be < st_at */
    st = vv_prng_advance(st, t - st_at); st_at = t;
    return st;
}
uint32_t vv_prng_read(uint64_t c) { return state_at(c ? c - 1 : 0); }
void vv_prng_write(uint64_t c, uint32_t dina) {
    stale = state_at(c); stale_at = c;
    st = dina ? dina : 1u; st_at = c + 1;
}
