/* hw.c - cycle-timed MIPS150 devices (see hw.h).  `now` = CPU cycles = instructions executed. */
#include "term.h"
#include "hw.h"
#include "quirk.h"
#include "memmap.h"
#include "display.h"
#include <stdio.h>
#include <time.h>

int      vv_legacy_io;
uint64_t vv_uart_rx_gap = VV_UART_RX_FRAME;
uint64_t vv_pf_period   = VV_PF_PERIOD;
int      vv_frame_clock = VV_FRAME_CLOCK_EMULATED;
uint32_t vv_cur_pc;

static uint64_t now_c;                     /* cycle of the last vv_hw_tick */
static uc_engine *g_uc;

/* ---------------- COP0150 (cpu/COP0150.v) ----------------
 * Interrupt sources are 1-cycle pulses OR-ed into Cause[15:10] every cycle (next_ip = ip | interrupts,
 * not masked by Status.IM, COP0150.v:52-53).  A Cause write keeps {wd[31:16], next_ip & wd[15:10],
 * wd[9:0]}: IP bits can only be cleared, never set (COP0150.v:91).  A Compare write clears IP15.
 * Reset: Count 0, Compare 0xFFFF, Status 0, Cause 0, EPC 0. */
uint32_t vv_hw_cause_ip;
static uint32_t status, epc, compare = 0xFFFFu, cnt0_val;
static uint64_t cnt0_at;                   /* cycle at which Count == cnt0_val */
static uint64_t hits_ip[6];
static void raise(uint32_t ip) {
    if (vv_legacy_io && ip != VV_IP_UARX) return;
    vv_hw_cause_ip |= ip;
    for (int b = 0; b < 6; b++) if (ip >> (10 + b) & 1) hits_ip[b]++;
}
static uint32_t count_at(uint64_t t) { return cnt0_val + (uint32_t)(t - cnt0_at); }

/* ---------------- UART (base_io/UAReceive.v, UATransmit.v, UARTRVA.v; cpu/MemMapIO.v) ----------------
 * RX: a byte from the host starts with its start bit at rx_start; HasByte is set 4340 cycles later
 * (10th symbol edge) and DataOut = RXShift[8:1].  DataOutValid = HasByte && !RXRunning, so it is low
 * while the NEXT byte is being shifted in, and the IRQ_RX edge (UARTRVA.v:30) comes once per landed
 * byte.  There is one holding register: a byte landing while HasByte is still set overwrites the
 * unread one (rx-overrun).  An RX_DATA read is DataOutReady: it clears HasByte whatever Valid is, and
 * during a reception it returns the partly shifted RXShift (junk).
 * TX: a TX_DATA store while DataInReady is low is lost (MemMapIO.v:101 "Loses a byte if Tx_Valid &&
 * !Tx_Ready"); an accepted byte keeps DataInReady low for 4773 cycles, then IRQ_TX (Tx_Ready rising). */
static int      rx_busy, rx_has;
static uint8_t  rx_byte;
static uint16_t rx_shift;                  /* RXShift[9:0]; power-up value unknown, taken as 0 */
static uint64_t rx_start, rx_land, rx_next_ok, rx_bytes, rx_overruns, rx_last_poll;
static uint64_t tx_ready_at, tx_sent, tx_lost; static int tx_irq_due;

static uint16_t rx_shift_now(void) {       /* RXShift as sampled so far (mid-symbol samples) */
    if (!rx_busy) return rx_shift;
    uint64_t dt = now_c - rx_start;
    unsigned k = dt < VV_UART_SYM / 2 ? 0 : (unsigned)((dt - VV_UART_SYM / 2) / VV_UART_SYM) + 1;
    if (k > 10) k = 10;
    uint16_t f = (uint16_t)(1u << 9 | (unsigned)rx_byte << 1), s = rx_shift;   /* stop, data, start(0) */
    for (unsigned i = 0; i < k; i++) s = (uint16_t)(((f >> i) & 1u) << 9 | s >> 1);
    return s;
}
uint32_t vv_uart_rx_valid(void) {
    if (vv_legacy_io) return vv_host_rx_avail(1);
    return rx_has && !rx_busy;
}
uint32_t vv_uart_rx_read(void) {
    if (vv_legacy_io) return vv_host_rx_avail(1) ? (uint32_t)vv_host_rx_take() : 0;
    uint32_t v = (rx_shift_now() >> 1) & 0xFFu;
    if (rx_has && vv_host_rx_paced()) rx_next_ok = now_c + vv_uart_rx_gap;   /* typed: next byte after a gap */
    rx_has = 0;                            /* DataOutReady clears HasByte (UAReceive.v:103) */
    return v;
}
uint32_t vv_uart_tx_ready(void) { return vv_legacy_io || now_c >= tx_ready_at; }
void vv_uart_tx_write(uint8_t c) {
    if (!vv_legacy_io && now_c < tx_ready_at) {
        tx_lost++;
        vv_quirk_report(VV_Q_TX_BUSY, vv_cur_pc, "UART TX store of 0x%02x while TX busy (%llu more cycles): HW loses the byte (MemMapIO.v:101); poll TX_READY (0x80000000) first",
                        c, (unsigned long long)(tx_ready_at - now_c));
        return;
    }
    vv_uart_out(c); tx_sent++;                 /* stdout = the serial line only (term.c) */
    if (!vv_legacy_io) { tx_ready_at = now_c + VV_UART_TX_BUSY; tx_irq_due = 1; }
}

/* ---------------- PixelFeeder (gpu/PixelFeeder.v) ----------------
 * A PF_FRAME store only latches frame_next (PixelFeeder.v:248); the fetch head switches framebits to
 * it when it finishes the frame being fetched (PixelFeeder.v:271-274) and fires irq_frame one cycle
 * later (interrupt_r, :247).  PF_FRAME reads return {4'h1, framebits_r, 22'b0} (MemMapIO.v:162), the
 * frame being scanned out, not the last one written.  vvizard puts the frame ends at k * period
 * (emulated cycles), or with --frame-clock=wall at t0 + k * 16.5792 ms of host CLOCK_MONOTONIC time, t0 =
 * the first instruction.  Wall mode reads the clock every VV_PF_WALL_CHECK instructions; deadlines missed
 * while the host was stalled are skipped (counted, no burst of catch-up frames), keeping the k * period phase. */
static uint32_t pf_cur, pf_next;
static uint64_t pf_boundary, pf_frames;   /* first frame end set at the first tick (from --pf-period) */
static uint64_t pf_t0, pf_deadline, pf_t_last, pf_skipped;   /* wall mode, ns */
uint64_t vv_mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t vv_pf_frames(void) { return pf_frames; }
static void pf_frame_end(uc_engine *uc) {
    pf_frames++;
    pf_cur = pf_next; vv_disp_pf_write(uc, pf_cur);   /* select + redraw on every frame end */
    raise(VV_IP_PF);
}
static uint32_t frame_bits(uint32_t f) { return (f >> 28) ? (f >> VV_FSHIFT) & VV_FNMASK : f & VV_FNMASK; }
void vv_pf_write(uc_engine *uc, uint32_t v) {
    pf_next = frame_bits(v);
    if (vv_legacy_io || !vv_pf_period) { pf_cur = pf_next; vv_disp_pf_write(uc, pf_cur); }
}
uint32_t vv_pf_frame(void) { return pf_cur; }

/* ---------------- GPU status (gpu/GPU.v, software/common/graphics.h:72-92) ----------------
 * GP_STATE = {pf_status, gp_status}.  pf_status = {fault, dormant, framebits_r[5:0], 8'b0}.
 * gp_status as software (graphics.h gstate_s, gios.c BC_GSTAT, GPUTestbench.v:24-27) reads it:
 * {gp_fault, 1'b0, gp_rframe[5:0], 3'b0, rect, elip, line, fill, gp_ready}.  NOTE: GPU.v:96-99 as
 * written concatenates 17 bits (4'b000 is a 4-bit literal) so the real hardware drops gp_fault and
 * shifts the frame to 14:9; vvizard models the intended layout (QUIRKS.md "gp-status").  The GPU
 * here completes instantly, so every engine reads ready; irq_gp_done is the gp_ready rising edge. */
static uint32_t gp_rframe, gp_fault; static uint64_t gp_launches;
void vv_gp_launched(uint32_t gp_frame, int rc) {
    gp_rframe = frame_bits(gp_frame); gp_fault = rc < -1; gp_launches++;
    if (!vv_legacy_io) raise(VV_IP_GP);
}
uint32_t vv_gp_status_word(void) {
    return (pf_cur & 0x3Fu) << 24 | gp_fault << 15 | (gp_rframe & 0x3Fu) << 8 | 0x1Fu;
}

/* ---------------- COP0 register access ---------------- */
uint32_t vv_hw_cause(void) {
    return vv_legacy_io ? (vv_hw_cause_ip & ~VV_IP_UARX) | (vv_host_rx_avail(0) ? VV_IP_UARX : 0) : vv_hw_cause_ip;
}
uint32_t vv_hw_status(void) { return status; }
void vv_hw_set_status(uint32_t st) { status = st; }
uint32_t vv_hw_epc(void) { return epc; }
void vv_hw_set_epc(uint32_t v) { epc = v; }
int vv_hw_isr_active;
static uint32_t isr_uncleared, isr_entry_pc; static uint64_t storms;
uint32_t vv_cop0_read(unsigned rd) {
    switch (rd) {
    case 9:  return count_at(now_c);
    case 11: return compare;
    case 12: return status;
    case 13: return vv_hw_cause();
    case 14: return epc;
    default: return 0;
    }
}
void vv_cop0_write(unsigned rd, uint32_t v) {
    switch (rd) {
    case 9:  cnt0_val = v; cnt0_at = now_c + 1; break;
    case 11: compare = v; vv_hw_cause_ip &= ~VV_IP_TIMER; isr_uncleared &= ~VV_IP_TIMER; break;
    case 12: status = v; break;
    case 13: vv_hw_cause_ip = (v & ~VV_IP_ALL) | (vv_hw_cause_ip & v & VV_IP_ALL); isr_uncleared &= vv_hw_cause_ip; break;
    default: break;                        /* EPC and the rest: ignored by COP0150 */
    }
}
int vv_hw_irq_request(uint32_t st) { return (st & 1u) && (st & vv_hw_cause() & VV_IP_ALL); }
int vv_hw_in_isr(void) { return vv_hw_isr_active; }
void vv_hw_isr_enter(uint32_t pc, uint32_t st) {
    epc = pc; status = st & ~1u;
    vv_hw_isr_active = 1; isr_entry_pc = pc;
    isr_uncleared = vv_hw_cause() & st & VV_IP_ALL;   /* the enabled, pending bits that caused entry */
}
/* irq-storm: the ISR returns (re-enables Status.IE, or runs eret) while a Cause bit that caused the
 * entry is still set and still enabled: COP0150 requests the interrupt again (taken two instructions later). */
void vv_hw_isr_exit_check(uint32_t st, uint32_t pc, const char *how) {
    if (!vv_hw_isr_active) return;
    if (st & 1u) vv_hw_isr_active = 0;
    uint32_t b = isr_uncleared & vv_hw_cause() & st & VV_IP_ALL;
    if (!b) return;
    storms++;
    vv_quirk_report(VV_Q_IRQ_STORM, pc, "%s with Cause 0x%04x still set and enabled (Status 0x%08x): bit(s) not cleared by the handler since the interrupt at pc=0x%08x, so the ISR is re-entered right away",
                    how, b, st, isr_entry_pc);
    isr_uncleared &= ~b;                   /* report each entry once */
}

/* ---------------- per-cycle step ---------------- */
void vv_hw_tick(uc_engine *uc, uint64_t now) {
    g_uc = uc;
    uint64_t prev = now_c; now_c = now;
    if (now <= prev && prev) return;
    /* COP0 timer (Count == Compare) and RTC (Count == 0xFFFFFFFF): any match in (prev, now] */
    if (!vv_legacy_io) {
        uint32_t c0 = count_at(prev), span = (uint32_t)(now - prev);
        if ((uint32_t)(compare - c0 - 1u) < span) raise(VV_IP_TIMER);
        if ((uint32_t)(0xFFFFFFFFu - c0 - 1u) < span) raise(VV_IP_RTC);
    }
    if (vv_legacy_io) return;
    /* UART RX: land the byte being received, then start the next host byte */
    if (rx_busy && now >= rx_land) {
        if (rx_has) {
            rx_overruns++;
            vv_quirk_report(VV_Q_RX_OVERRUN, vv_cur_pc, "UART RX byte 0x%02x was never read; it is overwritten by 0x%02x (one holding register, UAReceive.v:103-107)",
                            (rx_shift >> 1) & 0xFF, rx_byte);
        }
        rx_shift = (uint16_t)(1u << 9 | (unsigned)rx_byte << 1);
        rx_has = 1; rx_busy = 0; rx_bytes++;
        rx_next_ok = rx_land + vv_uart_rx_gap;
        raise(VV_IP_UARX);
    }
    if (!rx_busy && now >= rx_next_ok && !(rx_has && vv_host_rx_paced())) {
        int poll = now - rx_last_poll >= 4096;          /* stdin poll() at most every 4096 cycles */
        if (poll) rx_last_poll = now;
        if (vv_host_rx_avail(poll)) {
            rx_byte = (uint8_t)vv_host_rx_take(); rx_busy = 1; rx_start = now; rx_land = now + VV_UART_RX_FRAME;
        }
    }
    /* UART TX: ready again -> IRQ_TX */
    if (tx_irq_due && now >= tx_ready_at) { tx_irq_due = 0; raise(VV_IP_UATX); }
    /* PixelFeeder frame end: latch frame_next, irq_frame */
    if (!vv_pf_period) return;
    if (!pf_t0) {   /* first tick: phase origin of both clocks (the wall start also feeds the summary) */
        pf_t0 = pf_t_last = vv_mono_ns(); pf_deadline = pf_t0 + VV_PF_WALL_NS; pf_boundary = vv_pf_period;
    }
    if (vv_frame_clock == VV_FRAME_CLOCK_WALL) {
        if (now % VV_PF_WALL_CHECK) return;
        uint64_t t = pf_t_last = vv_mono_ns();
        if (t < pf_deadline) return;
        uint64_t late = (t - pf_deadline) / VV_PF_WALL_NS;   /* whole periods missed beyond this one */
        pf_skipped += late;
        pf_deadline += (late + 1) * VV_PF_WALL_NS;
        pf_frame_end(uc);
    } else if (now >= pf_boundary) {
        while (pf_boundary <= now) pf_boundary += vv_pf_period;
        pf_frame_end(uc);
    }
}

void vv_hw_summary(void) {
    if (vv_legacy_io) { vv_eprintf("[hw] --legacy-io: untimed UART, instant PF_FRAME, UARX-only interrupts\n"); return; }
    vv_eprintf("[hw] cycles %llu: UART tx %llu (lost %llu) rx %llu (overrun %llu); frames scanned %llu; "
            "Cause IP latched: uarx %llu uatx %llu gp %llu pf %llu rtc %llu timer %llu; irq storms %llu\n",
            (unsigned long long)now_c, (unsigned long long)tx_sent, (unsigned long long)tx_lost,
            (unsigned long long)rx_bytes, (unsigned long long)rx_overruns, (unsigned long long)pf_frames,
            (unsigned long long)hits_ip[0], (unsigned long long)hits_ip[1], (unsigned long long)hits_ip[2],
            (unsigned long long)hits_ip[3], (unsigned long long)hits_ip[4], (unsigned long long)hits_ip[5],
            (unsigned long long)storms);
    if (!vv_pf_period || !pf_t0) return;
    int wall = vv_frame_clock == VV_FRAME_CLOCK_WALL;
    double el = (double)((wall ? pf_t_last : vv_mono_ns()) - pf_t0) * 1e-9, rt = 1e9 / VV_PF_WALL_NS;
    double fps = el > 0 ? pf_frames / el : 0.0;
    char per[48];
    if (wall) snprintf(per, sizeof per, "%.4f ms", VV_PF_WALL_NS * 1e-6);
    else snprintf(per, sizeof per, "%llu cycles", (unsigned long long)vv_pf_period);
    vv_eprintf("[frame] clock %s (one frame per %s): %llu frames in %.3f s wall = %.2f/s (real time %.3f/s, x%.3f); "
               "%.2fM insns/s, %.0f insns/frame (hardware %u)", wall ? "wall" : "emulated", per,
               (unsigned long long)pf_frames, el, fps, rt, fps / rt, el > 0 ? now_c / el * 1e-6 : 0.0,
               pf_frames ? (double)now_c / pf_frames : 0.0, VV_PF_PERIOD);
    if (wall) vv_eprintf("; %llu deadlines skipped", (unsigned long long)pf_skipped);
    vv_eprintf("\n");
}
