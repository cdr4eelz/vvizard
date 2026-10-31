# uart_rxover.s - one RX holding register (run with XYZ on stdin and --uart-rx-gap 0): bytes that
# land before the previous one was read overwrite it (rx-overrun guest bug); RX_DATA ends as 'Z'.
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    li    $t1, 8000              # ~16000 cycles: all three bytes land (3 x 4340)
1:  addiu $t1, $t1, -1
    bne   $t1, $zero, 1b
    nop
    lw    $t2, RX_VALID($t0)
    nop
    EXPECT $t2, 1
    lw    $t1, RX_DATA($t0)
    nop
    EXPECT $t1, 'Z'
    break
