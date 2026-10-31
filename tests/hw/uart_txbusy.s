# uart_txbusy.s - a TX store while TX_READY is low is lost (MemMapIO.v:101): prints "A" only and
# trips the tx-busy guest-bug detector at the second store.
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    li    $t1, 'A'
    sw    $t1, TX_DATA($t0)
    li    $t1, 'B'
    sw    $t1, TX_DATA($t0)      # lost: TX still shifting 'A'
    lw    $t2, TX_READY($t0)
    nop
    EXPECT $t2, 0
    break
