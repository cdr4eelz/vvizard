# vv-load: 0x60000000
# vv-isa: -mips32r2
# Touch distinct DDR D$ lines; with --ddr-stalls, CNT_CYCLE exceeds CNT_INST.
        .set noreorder
        .text
        .globl _start
_start:
        lui   $t0, 0x8000
        sw    $zero, 0x18($t0)       # CNT_RESET

        lui   $t1, 0x1000            # DDR base 0x10000000
        lw    $zero, 0($t1)
        lw    $zero, 32($t1)
        lw    $zero, 64($t1)
        lw    $zero, 96($t1)
        lw    $zero, 128($t1)
        lw    $zero, 160($t1)
        lw    $zero, 192($t1)
        lw    $zero, 224($t1)
        lw    $zero, 0($t1)          # hit
        sw    $t1, 256($t1)          # write-through

        lw    $s0, 0x10($t0)         # CNT_CYCLE
        nop
        nop
        lw    $s1, 0x14($t0)         # CNT_INST
        nop
        nop
        subu  $s2, $s0, $s1          # delta (stalls)
        nop
        break
