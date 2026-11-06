# vv-load: 0x60000000
# vv-isa: -mips1
# rtl-diff: branch_delay
.set noreorder
.set noat
.include "check.inc"
.text
.global _start
_start:
    BEGIN_CHECKS

    li    $t0, 1
    li    $t2, 0
    beq   $t0, $t0, 1f
    addiu $t2, $zero, 0x11   # delay always runs
    addiu $t2, $zero, 0x22   # skipped
1:  CHECK $t2, 0x11

    END_CHECKS
