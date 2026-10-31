/* hw.h - cycle-timed MIPS150 devices: COP0150 interrupt latch (+ Count/Compare timer and RTC),
 * UART RX/TX (UAReceive.v / UATransmit.v / UARTRVA.v), PixelFeeder frame timing and the GPU status
 * word.  Time is the CPU cycle count, taken as the instruction count (1 CPI: vvizard has no stall
 * or cache-miss model).  RTL references are hardware/src (cs150 ISE fcdda7f / lcd-text).
 * --legacy-io restores the old untimed model (always-ready TX, level RX interrupt, instant
 * PF_FRAME, UARX the only interrupt). */
#ifndef VV_HW_H
#define VV_HW_H
#include <stdint.h>
#include <unicorn/unicorn.h>

#define VV_CPU_HZ        50000000u  /* ml505top.v:2 CPU_FREQ (PLL CLKOUT0 = 50 MHz, ml505top.v:269-273) */
#define VV_UART_BAUD     115200u    /* UARTRVA.v:10 (MemBank.v:223 passes only ClockFreq) */
#define VV_UART_SYM      (VV_CPU_HZ / VV_UART_BAUD)   /* 434 cycles: SymbolEdgeTime (UAReceive.v:27) */
#define VV_UART_RX_FRAME (10u * VV_UART_SYM)          /* 4340: start..stop bit, HasByte set on the 10th edge */
#define VV_UART_TX_BUSY  (11u * VV_UART_SYM - 1u)     /* 4773: BitCount 11 -> ready at BitCount==1 && SymbolEdge */
/* PixelFeeder: one frame = VESA 800x600@60 = 1056 x 628 pixel clocks at 40 MHz (ml505top.v:176, cross/VGA.v:10)
 * = 663168 clocks = 16.5792 ms (60.317 Hz) -> 828960 CPU cycles at 50 MHz (user decision; the DVI instance at
 * ml505top.v:184-185 is written with 1040 x 666, which gave the old default 865800, --pf-period 865800) */
#define VV_PF_HTOT       1056u
#define VV_PF_VTOT       628u
#define VV_PF_PIX_HZ     40000000u
#define VV_PF_PERIOD     ((uint32_t)((uint64_t)VV_PF_HTOT * VV_PF_VTOT * VV_CPU_HZ / VV_PF_PIX_HZ))   /* 828960 */
/* --frame-clock=wall: the same frame in host time, 663168 x 25 ns = 16579200 ns */
#define VV_PF_WALL_NS    ((uint64_t)VV_PF_HTOT * VV_PF_VTOT * 1000000000ull / VV_PF_PIX_HZ)
#define VV_PF_WALL_CHECK 1024u      /* wall mode: read the host clock every 1024 instructions (~0.05-0.1 ms) */
enum { VV_FRAME_CLOCK_EMULATED = 0, VV_FRAME_CLOCK_WALL = 1 };

/* Cause/Status interrupt bits (COP0150.v:43-44: {timer, rtc, pf_frame, gp_done, UATX, UARX} -> 15:10) */
#define VV_IP_UARX  0x0400u
#define VV_IP_UATX  0x0800u
#define VV_IP_GP    0x1000u
#define VV_IP_PF    0x2000u
#define VV_IP_RTC   0x4000u
#define VV_IP_TIMER 0x8000u
#define VV_IP_ALL   0xFC00u

extern int      vv_legacy_io;     /* --legacy-io */
extern uint64_t vv_uart_rx_gap;   /* --uart-rx-gap: idle cycles the host leaves between RX bytes */
extern uint64_t vv_pf_period;     /* --pf-period: cycles per scanned frame (0 = PF_FRAME acts at once) */
extern int      vv_frame_clock;   /* --frame-clock: VV_FRAME_CLOCK_EMULATED (default) or _WALL */
uint64_t vv_mono_ns(void);        /* host CLOCK_MONOTONIC in ns */
uint64_t vv_pf_frames(void);      /* frame events (PixelFeeder frame ends) so far */
extern uint32_t vv_cur_pc;        /* PC of the instruction being executed (for reports) */

void     vv_hw_tick(uc_engine *uc, uint64_t now);   /* advance devices to cycle `now` (every insn) */
/* COP0150 */
uint32_t vv_cop0_read(unsigned rd);
void     vv_cop0_write(unsigned rd, uint32_t v);
uint32_t vv_hw_cause(void);                         /* latched Cause (legacy: | RX level) */
uint32_t vv_hw_status(void);
void     vv_hw_set_status(uint32_t st);
uint32_t vv_hw_epc(void);
void     vv_hw_set_epc(uint32_t v);                 /* standard-MIPS mode only (cop0 quirk off) */
int      vv_hw_irq_request(uint32_t status);        /* ie & |(im & ip) (COP0150.v:56) */
void     vv_hw_isr_enter(uint32_t pc, uint32_t status);
void     vv_hw_isr_exit_check(uint32_t status, uint32_t pc, const char *how);   /* irq-storm detector */
int      vv_hw_in_isr(void);
extern uint32_t vv_hw_cause_ip;     /* the Cause latch (IP bits 15:10 and the rest) */
extern int      vv_hw_isr_active;   /* entered an ISR, not returned yet (irq-storm detector) */
/* 0 when no interrupt can be requested and there is no ISR exit to check: nothing latched in Cause
 * and not in an ISR (--legacy-io: Cause also reflects the host RX level, so always 1).  Lets
 * irq_step (main.c) skip reading Status for most instructions. */
static inline int vv_hw_irq_possible(void) {
    return vv_legacy_io || (vv_hw_cause_ip & VV_IP_ALL) || vv_hw_isr_active;
}
/* UART */
uint32_t vv_uart_tx_ready(void);
void     vv_uart_tx_write(uint8_t c);
uint32_t vv_uart_rx_valid(void);
uint32_t vv_uart_rx_read(void);                     /* RX_DATA read: data + DataOutReady (pops) */
/* PixelFeeder / GPU */
void     vv_pf_write(uc_engine *uc, uint32_t v);    /* PF_FRAME store */
uint32_t vv_pf_frame(void);                         /* frame being scanned out (framebits_r) */
void     vv_gp_launched(uint32_t gp_frame, int rc); /* after a GP_CODE launch (rc < -1: bad opcode fault) */
uint32_t vv_gp_status_word(void);                   /* GP_STATE (0x8000005C) read value */
void     vv_hw_summary(void);
/* provided by main.c: the host side of the serial line */
int      vv_host_rx_avail(int poll_stdin);
int      vv_host_rx_take(void);
int      vv_host_rx_paced(void);        /* next host byte waits for the guest to read the last */
#endif
