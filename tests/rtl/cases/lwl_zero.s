# vv-load: 0x60000000
# vv-isa: -mips1
# rtl-diff: lwl_zero
.set noreorder
.set noat
.include "check.inc"
.text
.global _start
_start:
    BEGIN_CHECKS

    # MIPS150 lwlr: lwl writes 0 (ByteAccess4 default for MemShift=1)
    lui   $s6, 0x5000
    ori   $s6, $s6, 0x0100
    li    $t0, 0x11223344
    sw    $t0, 0($s6)
    li    $t2, 0x99
    lwl   $t2, 1($s6); nop;  CHECK $t2, 0

    END_CHECKS
