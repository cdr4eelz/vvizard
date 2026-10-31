# check.inc - result-table macros for the probe programs (hwprobe.s and the hardware tests).
# Same instruction sequences as the cs150 Icarus CPU program tests use (CPU_prog_tb.v), written
# here so vvizard builds its tests without the cs150 tree.
# CHECK reg, exp  stores the pair (reg, exp) at $s7 and advances $s7 by 8 (uses $t9, $s7).
# END_CHECKS      stores the pair count to 0x50000004 and DONE_MAGIC to 0x50000000, then spins.
.equiv RESULTS,    0x50000400
.equiv DONE_MAGIC, 0x600DC0DE

.macro CHECK r, exp
    sw    \r, 0($s7)
    li    $t9, \exp
    sw    $t9, 4($s7)
    addiu $s7, $s7, 8
.endm

.macro BEGIN_CHECKS
    lui   $s7, %hi(RESULTS)
    ori   $s7, $s7, %lo(RESULTS)
.endm

.macro END_CHECKS
    lui   $t9, 0x5000
    lui   $t8, %hi(RESULTS)
    ori   $t8, $t8, %lo(RESULTS)
    subu  $t8, $s7, $t8
    srl   $t8, $t8, 3
    sw    $t8, 4($t9)
    li    $t8, DONE_MAGIC
    sw    $t8, 0($t9)
99: j     99b
    nop
.endm

# Address of a code label for code linked at 0x60000000 (offset < 64 KiB).
.macro LA_CODE r, label
    lui   \r, 0x6000
    ori   \r, \r, (\label - _start)
.endm
