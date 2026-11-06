# vv-load: 0x60000000
# vv-isa: -mips1
# rtl-diff: lb_sext — current CPUMIPS sign-extends lb/lh (MIPS-I); lb-sign quirk defaults OFF
.set noreorder
.set noat
.include "check.inc"
.text
.global _start
_start:
    BEGIN_CHECKS
    lui   $s6, 0x5000
    ori   $s6, $s6, 0x0100
    li    $t0, 0x80
    sb    $t0, 0($s6)
    lb    $t2, 0($s6); nop;  CHECK $t2, 0xffffff80
    li    $t0, 0x8001
    sh    $t0, 2($s6)
    lh    $t2, 2($s6); nop;  CHECK $t2, 0xffff8001
    END_CHECKS
