# vv-load: 0x60000000
# vv-isa: -mips1
# rtl-diff: mem_be
.set noreorder
.set noat
.include "check.inc"
.text
.global _start
_start:
    BEGIN_CHECKS

    lui   $s6, 0x5000
    ori   $s6, $s6, 0x0100
    li    $t0, 0x12345678
    sw    $t0, 0($s6)
    lw    $t2, 0($s6); nop;  CHECK $t2, 0x12345678
    lbu   $t2, 0($s6); nop;  CHECK $t2, 0x12
    lbu   $t2, 3($s6); nop;  CHECK $t2, 0x78
    lhu   $t2, 0($s6); nop;  CHECK $t2, 0x1234
    li    $t5, 0xAB
    sb    $t5, 1($s6)
    lw    $t2, 0($s6); nop;  CHECK $t2, 0x12AB5678

    END_CHECKS
