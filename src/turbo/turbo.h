/* Turbo is a fixed MIPS150 machine. No Unicorn or fidelity-option dependencies. */
#ifndef VV_TURBO_H
#define VV_TURBO_H
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include "memmap.h"

typedef struct turbo {
    uint8_t *ram[16]; uint32_t mask[16];
    uint32_t gpr[32], pc, count, compare, status, cause, epc, event_count;
    uint64_t event_insns, random_at, random_stale_at; uint32_t random_stale;
    int gp_pending;
    uint64_t insns, max_insns, max_ns, t0, event_at, pf_at, pf_frames, cnt_base;
    uint64_t tx_at, rx_at, rx_next, host_at, irqs;
    double mhz; /* 0 = unconstrained; else target Mcyc/s (== Minsn/s) */
    int tx_pending, rx_busy, rx_has; uint8_t rx_byte, rx_incoming;
    uint32_t pf_cur, pf_next, gp_frame, gp_code, gp_rframe, halt, halt_pc, random;
    uint64_t halt_stores, launches, drops;
    uint8_t lcd[32];
    int exit_on_halt, stopped, window, error;
    uint32_t load_old; int load_rt;
    /* All shared RAM accesses use aligned atomic words (including subword CAS).
     * This makes CPU/DMA/display overlap defined without a per-pixel mutex. */
    _Atomic uint64_t code_pages[1025];
    _Atomic unsigned epoch;
    pthread_t gpu_tid; sem_t gpu_wake;
    _Atomic int gpu_job, gpu_quit, gpu_fault;
    uint32_t job_frame, job_code;
    _Atomic int stop;
    void *cache;
} turbo;

typedef struct {
    uint32_t entry, sp;
    uint64_t max_insns, max_ns;
    double mhz;
    int window, exit_on_halt, dump_sm, dump_lcd;
    const char *dump_frame;
} turbo_opts;

int turbo_init(turbo *t);
void turbo_destroy(turbo *t);
int turbo_load(turbo *t, const char *path, uint32_t addr);
void turbo_cpu(turbo *t);
void turbo_events(turbo *t);
uint32_t turbo_mmio_read(turbo *t, uint32_t a);
void turbo_mmio_write(turbo *t, uint32_t a, uint32_t v);
int turbo_gpu_start(turbo *t);
void turbo_gpu_stop(turbo *t);
void turbo_gpu_kick(turbo *t);
int turbo_run(turbo *t, const turbo_opts *o);
void turbo_publish(turbo *t);
uint32_t turbo_sm_read(void *ctx, uint32_t a, unsigned n);

static inline uint32_t turbo_be(uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}
static inline uint8_t *turbo_ptr(turbo *t, uint32_t a, size_t n) {
    unsigned k = a >> 28; uint32_t off = a & t->mask[k];
    return t->ram[k] && n <= (uint64_t)t->mask[k] + 1 - off ? t->ram[k] + off : NULL;
}
static inline uint32_t turbo_word(const void *p) {
    return turbo_be(__atomic_load_n((const uint32_t *)p, __ATOMIC_RELAXED));
}
static inline unsigned turbo_page(uint32_t a) {
    unsigned k = a >> 28;
    if (k >= 1 && k <= 3) return (a & 0xfffffff) >> 12;
    return 65536 + (k == 4 ? 0 : k == 5 ? 4 : k == 6 ? 8 : 12) + ((a & 0x3fff) >> 12);
}
static inline void turbo_mark(turbo *t, uint32_t a) {
    unsigned p = turbo_page(a);
    atomic_fetch_or_explicit(&t->code_pages[p >> 6], 1ull << (p & 63), memory_order_relaxed);
}
static inline void turbo_invalidate(turbo *t, uint32_t a) {
    unsigned p = turbo_page(a);
    if (__builtin_expect(atomic_load_explicit(&t->code_pages[p >> 6], memory_order_relaxed) & (1ull << (p & 63)), 0))
        atomic_fetch_add_explicit(&t->epoch, 1, memory_order_release);
}
/* Spin-loop registers stay in the dispatcher's RAM/MMIO nibble fast path. */
static inline __attribute__((always_inline)) uint32_t turbo_read_io(turbo *t, uint32_t a) {
    switch (a & VV_MMIO_DECODE) {
    case VV_OW_CNT_CYCLE: case VV_OW_CNT_INST: return (uint32_t)(t->insns - t->cnt_base);
    case VV_OW_PF_FRAME: return VV_FRAME0 | (t->pf_cur << VV_FSHIFT);
    case VV_OW_GP_STATE: {
        int busy = atomic_load_explicit(&t->gpu_job, memory_order_acquire);
        int fault = atomic_load_explicit(&t->gpu_fault, memory_order_relaxed);
        return (t->pf_cur << 24) | (fault ? 0x8000u : 0) | (t->gp_rframe << 8) | (busy ? 0 : 31);
    }
    default: return turbo_mmio_read(t, a);
    }
}
static inline __attribute__((always_inline)) uint32_t turbo_load_mem(turbo *t, uint32_t a, unsigned size, int sign) {
    uint32_t v = 0;
    if ((a >> 28) == 8) { v = turbo_read_io(t, a); if (a & (size - 1)) return 0; }
    else if (!(a & (size - 1))) {
        const uint8_t *p = turbo_ptr(t, a & ~3u, 4);
        if (p) v = turbo_word(p);
    }
    if (size < 4) {
        v >>= 8 * (4 - size - (a & (4 - size)));
        v &= size == 1 ? 255u : 65535u;
        if (sign) v = size == 1 ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v;
    }
    return v;
}
static inline __attribute__((always_inline)) void turbo_store_mem(turbo *t, uint32_t a, unsigned size, uint32_t v) {
    unsigned k = a >> 28;
    if (k == 8) {
        if (a & (size - 1)) { (void)turbo_mmio_read(t, a); return; }
        if (size < 4) v = (v & (size == 1 ? 255u : 65535u)) << (8 * (4 - size - (a & (4 - size))));
        turbo_mmio_write(t, a, v); return;
    }
    if (k == 4 || (k == 2 && (t->pc >> 28) != 4) || (a & (size - 1))) return;
    uint8_t *p = turbo_ptr(t, a & ~3u, 4); if (!p) return;
    if (size == 4) __atomic_store_n((uint32_t *)p, turbo_be(v), __ATOMIC_RELAXED);
    else {
        unsigned sh = 8 * (4 - size - (a & (4 - size)));
        uint32_t mask = (size == 1 ? 255u : 65535u) << sh;
        uint32_t old = __atomic_load_n((uint32_t *)p, __ATOMIC_RELAXED), next;
        do { next = turbo_be((turbo_be(old) & ~mask) | ((v << sh) & mask)); }
        while (!__atomic_compare_exchange_n((uint32_t *)p, &old, next, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }
    turbo_invalidate(t, a);
}
static inline unsigned turbo_frame(uint32_t f) { return f >> 28 ? (f >> 22) & 63 : f & 63; }
#endif
