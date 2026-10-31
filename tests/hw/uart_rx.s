# uart_rx.s - UART RX (run with --uart-in AB): IRQ_RX is the rising edge of Rx_Valid, latched in
# Cause bit 10 until software clears it (it stays set after the byte is read); Cause IP bits can
# only be cleared by mtc0, never set.
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x0400
    EXPECT $t3, 0                # nothing received yet (a byte takes 4340 cycles)
    li    $t1, 0x0400
    mtc0  $t1, $13               # try to SET bit 10: COP0150 only lets mtc0 clear IP bits
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x0400
    EXPECT $t3, 0
1:  lw    $t2, RX_VALID($t0)
    nop
    beq   $t2, $zero, 1b
    nop
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x0400
    EXPECT $t3, 0x0400           # edge latched
    lw    $t1, RX_DATA($t0)
    nop
    EXPECT $t1, 'A'
    lw    $t2, RX_VALID($t0)
    nop
    EXPECT $t2, 0                # read clears HasByte
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x0400
    EXPECT $t3, 0x0400           # still latched: not a level
    mtc0  $zero, $13
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x0400
    EXPECT $t3, 0
2:  mfc0  $t3, $13               # wait for the second byte's edge
    nop
    andi  $t3, $t3, 0x0400
    beq   $t3, $zero, 2b
    nop
    lw    $t1, RX_DATA($t0)
    nop
    EXPECT $t1, 'B'
    break
