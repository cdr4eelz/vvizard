# vv-load: 0x60000000
# vv-isa: -mips1
# rtl-diff: alu_shifts
.set noreorder
.set noat
.include "check.inc"
.text
.global _start
_start:
    BEGIN_CHECKS

    li    $t0, 0x80000000
    sll   $t2, $t0, 1;     CHECK $t2, 0
    srl   $t2, $t0, 31;    CHECK $t2, 1
    sra   $t2, $t0, 31;    CHECK $t2, 0xffffffff
    li    $t3, -5
    sra   $t2, $t3, 1;     CHECK $t2, 0xfffffffd
    li    $t4, 4
    sllv  $t2, $t0, $t4;   CHECK $t2, 0
    srlv  $t2, $t0, $t4;   CHECK $t2, 0x08000000

    END_CHECKS
