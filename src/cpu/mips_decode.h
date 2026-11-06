/* mips_decode.h - instruction-field helpers shared by the Unicorn quirk path and the fast2 backend.
 * These match the historical copies in quirk.c / main.c.  IRQ deferral uses vv_is_branch (every
 * REGIMM, including traps).  Load-use / delay-slot detection uses vv_is_cti (real control transfers
 * only: REGIMM rt with bits [3:2] clear, i.e. bltz/bgez/likely and the al forms). */
#ifndef VV_MIPS_DECODE_H
#define VV_MIPS_DECODE_H
#include <stdint.h>

static inline uint32_t vv_reads(uint32_t i) {
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, fn = i & 63;
    uint32_t S = 1u << rs, T = 1u << rt;
    switch (op) {
    case 0:
        if (fn <= 3) return T;
        if (fn == 8 || fn == 9 || fn == 17 || fn == 19) return S;
        if (fn == 12 || fn == 13 || fn == 16 || fn == 18) return 0;
        return S | T;
    case 1: case 6: case 7: return S;
    case 2: case 3: case 15: return 0;
    case 4: case 5: return S | T;
    case 0x10: return rs == 4 ? T : 0;
    default:
        if (op >= 8 && op <= 14) return S;
        if (op == 0x22 || op == 0x26) return S | T;
        if (op >= 0x20 && op <= 0x27) return S;
        if (op >= 0x28 && op <= 0x2F) return S | T;
        return 0;
    }
}
static inline uint32_t vv_writes(uint32_t i) {
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31, fn = i & 63;
    switch (op) {
    case 0:
        if (fn == 8 || fn == 12 || fn == 13 || fn == 17 || fn == 19 || (fn >= 24 && fn <= 27)) return 0;
        return 1u << rd;
    case 1: return (rt & 0x10) ? 1u << 31 : 0;
    case 3: return 1u << 31;
    case 0x10: return rs == 0 ? 1u << rt : 0;
    default:
        if ((op >= 8 && op <= 15) || (op >= 0x20 && op <= 0x27)) return 1u << rt;
        return 0;
    }
}
/* Value operands: reads() minus a store's data register and minus xor/sub/subu rd,rs,rs. */
static inline uint32_t vv_value_reads(uint32_t i) {
    unsigned op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, fn = i & 63;
    if (op >= 0x28 && op <= 0x2E) return 1u << rs;
    if (op == 0 && rs == rt && (fn == 34 || fn == 35 || fn == 38)) return 0;
    return vv_reads(i);
}
static inline int vv_is_cti(uint32_t i) {
    unsigned op = i >> 26, rt = (i >> 16) & 31, fn = i & 63;
    return (op == 0 && (fn == 8 || fn == 9)) || (op == 1 && (rt & 0xC) == 0)
        || (op >= 2 && op <= 7) || (op >= 20 && op <= 23);
}
/* CPUMIPS.v DO_ISR: any REGIMM counts as a branch, as do cop0 mtc0 and the compact branches. */
static inline int vv_is_branch(uint32_t i) {
    unsigned op = i >> 26, fn = i & 0x3F;
    return (op == 0 && (fn == 8 || fn == 9)) || op == 1 || (op >= 2 && op <= 7) || (op >= 20 && op <= 23);
}
static inline int vv_is_mtc0(uint32_t i) { return (i >> 21) == 0x204; }

static inline int vv_known_special(unsigned fn) {
    static const uint64_t ok = (1ull << 0) | (1ull << 2) | (1ull << 3) | (1ull << 4) | (1ull << 6) | (1ull << 7) |
        (1ull << 8) | (1ull << 9) | (1ull << 12) | (1ull << 13) | (0xFull << 16) | (0xFull << 24) |
        (0xFFull << 32) | (1ull << 42) | (1ull << 43);
    return (int)((ok >> fn) & 1ull);
}
#endif
