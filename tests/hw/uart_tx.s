# uart_tx.s - UART TX timing (UATransmit.v, 115200 baud at 50 MHz): TX_READY drops for 4773 cycles
# after an accepted byte, then IRQ_TX latches Cause bit 11 (COP0150.v).  Prints "AB" (no tx-busy).
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    mtc0  $zero, $13
    lw    $t1, TX_READY($t0)
    nop
    EXPECT $t1, 1                # idle: ready
    li    $t1, 'A'
    mfc0  $s0, $9                # Count before the store
    sw    $t1, TX_DATA($t0)
    lw    $t2, TX_READY($t0)
    nop
    EXPECT $t2, 0                # busy right after the store
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x0800
    EXPECT $t3, 0                # no TX interrupt yet
1:  lw    $t2, TX_READY($t0)
    nop
    beq   $t2, $zero, 1b
    nop
    mfc0  $s1, $9                # Count when ready again
    nop
    subu  $s1, $s1, $s0
    INRANGE $t4, $s1, 4773, 4790 # 11 symbol edges of 434 cycles (+ the poll loop)
    EXPECT $t4, 1
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x0800
    EXPECT $t3, 0x0800           # IRQ_TX latched in Cause bit 11
    mtc0  $zero, $13             # Cause write clears it
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x0800
    EXPECT $t3, 0
    li    $t1, 'B'
    sw    $t1, TX_DATA($t0)
    break
