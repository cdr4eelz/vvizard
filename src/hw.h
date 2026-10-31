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
/* PixelFeeder: one frame = 1040 x 666 DVI clocks (ml505top.v:184-185) at 40 MHz = 692640 -> 865800 CPU cycles */
#define VV_PF_PERIOD     (1040u * 666u * (VV_CPU_HZ / 1000000u) / 40u)

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
