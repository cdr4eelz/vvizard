/* prng.h - PRNG at MMIO 0x80000080 (MemMapIO.v A_Random32 + base_util/PRNG32LFSR.v).
 * 32-bit Galois LFSR, mask 0x80200003, reset value 0xACE12468, steps EVERY clock (not per read).
 * Time base is the emulator's cycle count (vvizard: cycles == instructions executed). */
#ifndef VV_PRNG_H
#define VV_PRNG_H
#include <stdint.h>

#define VV_PRNG_MASK  0x80200003u
#define VV_PRNG_RESET 0xACE12468u

/* MMIO read issued in cycle c: returns the value the read port captures (prng_out_r = state(c-1)). */
uint32_t vv_prng_read(uint64_t c);
/* MMIO store issued in cycle c with write data dina (already placed in its byte lane):
 * state(c+1) = dina ? dina : 1.  A read in cycle c+1 still sees the old state(c). */
void     vv_prng_write(uint64_t c, uint32_t dina);
/* LFSR advanced n steps from s (O(log n) via GF(2) matrix powers). */
uint32_t vv_prng_advance(uint32_t s, uint64_t n);
#endif
