# vv-load: 0x60000000
# vv-isa: -mips1
# rtl-diff: alu_wrap
.set noreorder
.set noat
.include "check.inc"
.text
.global _start
_start:
    BEGIN_CHECKS

    # MIPS150 overflow quirk: addi/add wrap (no trap). Expect wrap result.
    lui   $t0, 0x7fff
    ori   $t0, $t0, 0xffff
    addiu $t2, $t0, 1;     CHECK $t2, 0x80000000

    END_CHECKS
