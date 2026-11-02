# halt_val.s - the HaltVal register (0x80000084, one word past RANDOM32: MemMapIO.v A_HaltVal = 6'h21,
# mmio_intr_cop0.h OW_HALTVAL; on the FPGA a plain read/write register, reset 0).
# Default (no --exit-on-halt): a plain store-then-load register.  Stores 42 and reads it back, then
# sb/sh (lane-placed: sb at +3 = the byte, sh at +2 = the halfword) and lhu/lbu lanes, ends with 99
# stored, then does what FPGA code does next: interrupts off (Status IE = 0) and spin (--max-insns).
# With --exit-on-halt vvizard stops right after the first store ($s0 still 0, $s3 = 0).
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    move  $s0, $zero
    li    $t1, 42
    sw    $t1, HALTVAL($t0)      # --exit-on-halt stops here
    li    $s0, 1                 # default (and FPGA): carry on
    lw    $t2, HALTVAL($t0)
    nop
    EXPECT $t2, 42               # 1: reads back
    li    $t1, 7
    sb    $t1, HALTVAL+3($t0)    # low lane: word 0x00000007
    lw    $t2, HALTVAL($t0)
    nop
    EXPECT $t2, 7                # 2
    li    $t1, 0x1234
    sh    $t1, HALTVAL+2($t0)    # low halfword: word 0x00001234
    lw    $t2, HALTVAL($t0)
    nop
    EXPECT $t2, 0x1234           # 3
    lhu   $t2, HALTVAL+2($t0)
    nop
    EXPECT $t2, 0x1234           # 4
    lbu   $t2, HALTVAL+3($t0)
    nop
    EXPECT $t2, 0x34             # 5
    lbu   $t2, HALTVAL+2($t0)
    nop
    EXPECT $t2, 0x12             # 6
    li    $t1, 0xdeadbeef
    sw    $t1, HALTVAL($t0)
    lw    $t2, HALTVAL($t0)
    nop
    EXPECT $t2, 0xdeadbeef       # 7
    li    $t1, 99
    sw    $t1, HALTVAL($t0)      # the last value: printed after the register dump
    lw    $t2, HALTVAL($t0)
    nop
    EXPECT $t2, 99               # 8
    mfc0  $t2, $12
    nop
    li    $t3, ~1
    and   $t2, $t2, $t3
    mtc0  $t2, $12               # Status.IE = 0: no interrupts
1:  b     1b                     # spin forever
    nop
