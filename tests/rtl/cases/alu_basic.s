# vv-load: 0x60000000
# vv-isa: -mips1
# rtl-diff: alu_basic
.set noreorder
.set noat
.include "check.inc"
.text
.global _start
_start:
    BEGIN_CHECKS

    li    $t0, 0x12345678
    li    $t1, 0x0F0F0F0F
    addu  $t2, $t0, $t1;   CHECK $t2, 0x21436587
    subu  $t2, $t0, $t1;   CHECK $t2, 0x03254769
    and   $t2, $t0, $t1;   CHECK $t2, 0x02040608
    or    $t2, $t0, $t1;   CHECK $t2, 0x1F3F5F7F
    xor   $t2, $t0, $t1;   CHECK $t2, 0x1D3B5977
    nor   $t2, $t0, $t1;   CHECK $t2, 0xE0C0A080
    slt   $t2, $t1, $t0;   CHECK $t2, 1
    sltu  $t2, $t0, $t1;   CHECK $t2, 0

    END_CHECKS
