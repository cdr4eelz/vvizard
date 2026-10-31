# dcache.s - the D-cache (Cache.v: direct-mapped, 256 x 32-byte lines, write-through, no flush) is
# not snooped: after a GPU write to a DDR line the CPU has cached, a load hits the old data
# (dcache-stale guest bug).  Run with --no-random-ddr (DDR starts 0).  $s0 = load after the FILL
# (HW: stale 0), $s1 = load after evicting the line (new colour).
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    lui   $s2, 0x1000            # frame 0, pixel (0,0): 0x10000000 (D$ path)
    lw    $t1, 0($s2)            # miss: line filled (0)
    nop
    EXPECT $t1, 0
    lui   $s6, 0x1200
    ori   $s6, $s6, 0x3000
    li    $t1, 0x10345678        # FILL 0x345678
    sw    $t1, 0($s6)
    sw    $zero, 4($s6)          # STOP
    sw    $s2, GP_FRAME($t0)
    sw    $s6, GP_CODE($t0)      # GPU fills frame 0 behind the cache
    lw    $s0, 0($s2)            # hits the stale line
    nop
    lw    $t1, 0x2000($s2)       # same index, other tag: evicts the line
    nop
    lw    $s1, 0($s2)            # refilled from DDR
    nop
    EXPECT $s1, 0x345678
    break
