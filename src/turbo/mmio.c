#include "turbo.h"
#include "hw.h"
#include "term.h"
#include "display.h"
#include "prng.h"
#include <limits.h>

/* Cold devices are out of the dispatch graph. Events are deadline-based;
 * Count is advanced in bulk by the CPU, not by a per-cycle callback. */
void turbo_events(turbo *t) {
    uint64_t delta = t->insns - t->event_insns;
    if ((uint32_t)(t->compare - t->event_count - 1) < delta) t->cause |= VV_IP_TIMER;
    if ((uint32_t)(0xffffffffu - t->event_count - 1) < delta) t->cause |= VV_IP_RTC;
    t->event_count = t->count; t->event_insns = t->insns;
    if (t->tx_pending && t->insns >= t->tx_at) { t->tx_pending = 0; t->cause |= VV_IP_UATX; }
    if (t->rx_busy && t->insns >= t->rx_at) {
        t->rx_busy = 0; t->rx_has = 1; t->rx_next = t->rx_at + VV_UART_RX_FRAME; t->cause |= VV_IP_UARX;
    }
    if (t->insns >= t->host_at) {
        t->host_at = t->insns + 4096;
        if (!t->rx_busy && t->insns >= t->rx_next && !(t->rx_has && vv_host_rx_paced()) && vv_host_rx_avail(1)) {
            t->rx_byte = vv_host_rx_take(); t->rx_busy = 1; t->rx_at = t->insns + VV_UART_RX_FRAME;
        }
        if (t->max_ns && vv_mono_ns() - t->t0 >= t->max_ns) t->stopped = 1;
        vv_term_tick();
        if (t->window) turbo_publish(t);
    }
    if (t->insns >= t->pf_at) {
        do { t->pf_at += VV_PF_PERIOD; t->pf_frames++; } while (t->pf_at <= t->insns);
        t->pf_cur = t->pf_next; t->cause |= VV_IP_PF;
    }
    if (t->gp_pending && !atomic_load_explicit(&t->gpu_job, memory_order_acquire)) {
        t->cause |= VV_IP_GP; t->gp_pending = 0; /* sampled completion edge, not a wait */
    }
    uint64_t next = t->pf_at;
#define EARLIER(v) do { uint64_t x_ = (v); if (x_ > t->insns && x_ < next) next = x_; } while (0)
    EARLIER(t->host_at);
    if (t->rx_busy) EARLIER(t->rx_at);
    if (t->tx_pending) EARLIER(t->tx_at);
    uint32_t d = t->compare - t->count; EARLIER(t->insns + (d ? (uint64_t)d : 0x100000000ull));
    d = 0xffffffffu - t->count; EARLIER(t->insns + (d ? (uint64_t)d : 0x100000000ull));
    if (atomic_load_explicit(&t->gpu_job, memory_order_relaxed)) EARLIER(t->insns + 64);
    t->event_at = next;
#undef EARLIER
}

static uint32_t random_at(turbo *t, uint64_t cycle) {
    if (cycle < t->random_at) return cycle == t->random_stale_at ? t->random_stale : t->random;
    t->random = vv_prng_advance(t->random, cycle - t->random_at); t->random_at = cycle;
    return t->random;
}

uint32_t turbo_mmio_read(turbo *t, uint32_t a) {
    switch (a & VV_MMIO_DECODE) {
    case VV_OW_UATX_READY: return t->insns >= t->tx_at;
    case VV_OW_UARX_VALID: return t->rx_has && !t->rx_busy;
    case VV_OW_UARX_DATA: t->rx_has = 0; t->rx_next = t->insns + VV_UART_RX_FRAME; return t->rx_byte;
    case VV_OW_CNT_CYCLE: case VV_OW_CNT_INST: return (uint32_t)(t->insns - t->cnt_base);
    case VV_OW_PF_FRAME: return VV_FRAME0 | (t->pf_cur << VV_FSHIFT);
    case VV_OW_GP_STATE: return (t->pf_cur << 24) | (atomic_load_explicit(&t->gpu_fault, memory_order_relaxed) ? 0x8000u : 0) |
        (t->gp_rframe << 8) | (atomic_load_explicit(&t->gpu_job, memory_order_acquire) ? 0 : 31);
    case VV_OW_HALTVAL: return t->halt;
    case VV_OW_RANDOM32: return random_at(t, t->insns ? t->insns - 1 : 0);
    default: return 0;
    }
}
__attribute__((noinline)) void turbo_mmio_write(turbo *t, uint32_t a, uint32_t v) {
    unsigned off = a & VV_MMIO_DECODE;
    switch (off) {
    case VV_OW_UATX_DATA:
        if (t->insns >= t->tx_at) { vv_uart_out((uint8_t)v); t->tx_at = t->insns + VV_UART_TX_BUSY; t->tx_pending = 1; }
        break;
    case VV_OW_CNT_RESET: t->cnt_base = t->insns; break;
    case VV_OW_PF_FRAME: t->pf_next = turbo_frame(v); break;
    case VV_OW_GP_FRAME: t->gp_frame = v; break;
    case VV_OW_GP_GCODE: t->gp_code = v & ~3u; turbo_gpu_kick(t); break;
    case VV_OW_RANDOM32:
        t->random_stale = random_at(t, t->insns); t->random_stale_at = t->insns;
        t->random = v ? v : 1; t->random_at = t->insns + 1; break;
    case VV_OW_HALTVAL:
        t->halt = v; t->halt_pc = t->pc; t->halt_stores++;
        if (t->exit_on_halt) t->stopped = 1;
        break;
    default:
        if (off >= VV_OW_LCD_BASE && off < VV_OW_LCD_END) t->lcd[(off - VV_OW_LCD_BASE) / 4] = v;
    }
    t->event_at = t->insns + 1;
}
