# vv-load: 0x60000000
# vv-isa: -mips1
# rtl-diff: jal_link
.set noreorder
.set noat
.include "check.inc"
.text
.global _start
_start:
    BEGIN_CHECKS

    jal   1f
    nop                      # delay; return lands here at 0x60000008
    move  $t2, $ra
    CHECK $t2, 0x60000010
    j     2f
    nop
1:  jr    $ra
    nop
2:

    END_CHECKS
