/* rtl.c - approximate instruction- and data-cache stalls for --cpu=rtl.
 *
 * This is not cycle-exact.  Cache.v is not in this snapshot.  The model follows what is:
 *   cpu/MemBank.v: fetch nibbles 0x1/0x2/0x3 go through the I-cache; data nibbles 0x1/0x2/0x3
 *     go through the D-cache; BIOS 0x4, D-scratch 0x5, I-scratch 0x6 and ISR 0xC are BRAM
 *     (not the cache path); an MMIO fetch is DEAD_IMEM (0).
 *   cache.vh geometry, as already used by src/dcache.h: direct-mapped, 256 lines x 32 bytes,
 *     index addr[12:5], tag addr[27:13].
 *   hardware/sim-icarus/tb/CPU_prog_tb.v NSTALL default 4: one miss (and one committed DDR
 *     store) costs 4 cycles.  MIPS150.v COP0 .ena(1'b1), so Count advances on those stalls.
 *
 * Separate I and D tag arrays.  A store does not snoop or invalidate the I-cache and does not
 * allocate in the D-cache.  A committed store to a cacheable DDR nibble (raw 0x1 or 0x3, or
 * 0x2 when the ddr2 drop does not apply) costs 4 cycles even on a hit.  A cacheable load
 * costs 4 only on a miss, then fills.  Dropped stores (BIOS, ddr2 from outside the BIOS,
 * unmapped, unaligned, lwl/lwr/swl/swr, reserved address 0) cost nothing and do not fill.
 * BRAM and MMIO cost nothing.  An I-miss and a D-miss on the same instruction cost 8.
 * IRQ is not sampled during stall cycles.  --rtl-cache=off forces every penalty to 0, so
 * cycles stay equal to the instruction count.
 *
 * The address used for the D-cache lookup is the architectural base+offset (no load-use
 * substitution).  A delay-slot instruction that a branch-likely would have nullified is still
 * looked up.  Both are approximations.  The functional dcache-stale model (src/dcache.c)
 * is untouched and is not these tags.
 */
#include "rtl.h"
#include "cpu/fast/fast.h"
#include "cpu/fast/decode.h"
#include "cpu/service.h"
#include "cpu/irq.h"
#include "cpu/backend.h"
#include "hw.h"
#include "quirk.h"
#include "memmap.h"
#include <string.h>

#define VV_RTL_STALL 4
#define VV_RTL_LINES 256u

extern int vv_rtl_cache;

typedef struct { uint32_t tag; uint8_t valid; } RtlLine;
static RtlLine icache[VV_RTL_LINES], dcache_t[VV_RTL_LINES];

static int lookup(RtlLine *a, uint32_t addr, int fill) {
    unsigned idx = (addr >> 5) & (VV_RTL_LINES - 1);
    uint32_t tag = (addr >> 13) & 0x7FFFu;
    if (a[idx].valid && a[idx].tag == tag) return 0;
    if (fill) { a[idx].valid = 1; a[idx].tag = tag; }
    return VV_RTL_STALL;
}

static void stall_cycles(uc_engine *uc, int n) {
    for (int i = 0; i < n; i++) {
        if (vv_guest_stopping() || vv_quirk_halted) return;
        vv_hw_tick(uc, vv_hw_now() + 1);   /* Count advances; the insn is not retired yet */
    }
}

static int ic_stall(uint32_t pc) {
    if (!vv_rtl_cache || (pc & 3)) return 0;
    unsigned nb = pc >> 28;
    if (nb != 1 && nb != 2 && nb != 3) return 0;   /* BRAM / MMIO fetch: no I-cache */
    return lookup(icache, pc, 1);
}

/* 1 when the hardware drops the access (no cache fill, no stall). */
static int mem_dropped(const Dec *d, uint32_t addr) {
    int store = (d->flags & FL_STORE) != 0;
    if ((d->flags & FL_ADDR0) && vv_quirk_on(VV_Q_RESERVED)) return 1;
    if ((d->flags & (FL_LWL | FL_LWR | FL_SWL | FL_SWR)) && vv_quirk_on(VV_Q_LWLR)) return 1;
    unsigned size = d->alu;
    if (size > 1 && (addr & (size - 1)) &&
        !(d->flags & (FL_LWL | FL_LWR | FL_SWL | FL_SWR)) && vv_quirk_on(VV_Q_UNALIGNED))
        return 1;
    if (vv_canon_inl(addr) == 0xFFFFFFFFu && vv_quirk_on(VV_Q_UNMAPPED)) return 1;
    unsigned nb = addr >> 28;
    if (store && nb == 4 && vv_quirk_on(VV_Q_BIOS_STORE)) return 1;
    if (store && nb == 2 && (d->pc >> 28) != 4 && vv_quirk_on(VV_Q_DDR2)) return 1;
    return 0;
}

static int dc_stall(uint32_t pc) {
    if (!vv_rtl_cache) return 0;
    Dec d;
    vv_decode_at(pc, &d);
    if (d.op != OP_MEM) return 0;
    int store = (d.flags & FL_STORE) != 0;
    uint32_t addr = (d.rs ? vv_fast_gpr(d.rs) : 0) + (uint32_t)d.imm;
    if ((d.flags & FL_ADDR0) && vv_quirk_on(VV_Q_RESERVED)) addr = 0;
    if (mem_dropped(&d, addr)) return 0;
    unsigned nb = addr >> 28;
    if (nb != 1 && nb != 2 && nb != 3) return 0;   /* BRAM / MMIO: not the D-cache */
    if (store) return VV_RTL_STALL;                /* write-through, no allocate, no I$ snoop */
    return lookup(dcache_t, addr, 1);
}

static void (*bb_inval)(uint32_t lo, uint32_t hi);

static void rtl_inval(uint32_t lo, uint32_t hi) {
    if (bb_inval && bb_inval != rtl_inval) bb_inval(lo, hi);
    /* GPU (or another host writer) changed bytes the I-cache may hold.  Drop overlapping lines.
     * CPU stores do not come through here and do not snoop the I-cache. */
    if (hi < lo) return;
    for (uint32_t a = lo & ~31u; a < hi; ) {
        unsigned nb = a >> 28;
        if (nb == 1 || nb == 2 || nb == 3) {
            unsigned idx = (a >> 5) & (VV_RTL_LINES - 1);
            uint32_t tag = (a >> 13) & 0x7FFFu;
            if (icache[idx].valid && icache[idx].tag == tag) icache[idx].valid = 0;
        }
        uint32_t n = a + 32;
        if (n <= a) break;
        a = n;
    }
}

uc_err vv_rtl_run(uc_engine *uc, uint32_t entry, uint32_t until, int has_until) {
    memset(icache, 0, sizeof icache);
    memset(dcache_t, 0, sizeof dcache_t);
    vv_fast_init(uc, entry);
    bb_inval = vv_code_invalidate;
    vv_code_invalidate = rtl_inval;
    uint32_t pc = entry;
    while (!vv_quirk_halted) {
        if (has_until && pc == until) break;
        stall_cycles(uc, ic_stall(pc) + dc_stall(pc));
        if (vv_guest_stopping() || vv_quirk_halted) break;
        if (vv_fast_fetch_aborts(pc)) return UC_ERR_EXCEPTION;
        if (vv_svc_enter_at(uc, pc, vv_hw_now() + 1)) break;
        uint32_t npc = pc;
        if (vv_irqs_on() && vv_irq_step(NULL, pc, &npc)) {
            vv_fast_cancel_lu();
            vv_quirk_cancel();
            if (!vv_quirk_halted && vv_svc_leave(uc, pc)) break;
            if (vv_quirk_halted) break;
            pc = npc;
            continue;
        }
        if (vv_quirk_halted) break;
        if (vv_svc_leave(uc, pc)) break;
        if (vv_fast_step(uc, &pc)) break;
    }
    /* Same as vv_fast_run: a printed CPU exception returns OK; a refused fetch does not. */
    if (vv_fast_unhandled()) return UC_ERR_EXCEPTION;
    return UC_ERR_OK;
}
