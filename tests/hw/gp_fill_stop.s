# gp_fill_stop.s - GraphicsProcessor details checked against the RTL:
#  FILL (FrameFiller.v:90-91): one ScanLineRunner run per row with edge color {4'b0, COLOR28} at
#    x=0 and x=799 and fill color 0xFF000000 | color in between (stored words, not just RGB).
#  STOP (GraphicsProcessor.v:182-189): "Valid STOP must be all zeros"; any other bit set stops the
#    list and sets GP_fault (GP_STATE bit 15), like a bad opcode.
# vv-load: 0x40000000
# vv-isa: -mips1
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    lui   $s0, 0x1200
    ori   $s0, $s0, 0x3000       # command list in DDR (graphics.h GPTEMP)
    li    $t1, 0x10123456        # FILL COLOR28 0x0123456
    sw    $t1, 0($s0)
    sw    $zero, 4($s0)          # STOP
    li    $t1, 0x10400000        # frame 1
    sw    $t1, GP_FRAME($t0)
    sw    $s0, GP_CODE($t0)      # launch
1:  lw    $t1, GP_STATE($t0)     # GP_WAIT (always ready with --sync)
    nop
    andi  $t1, $t1, 1
    beq   $t1, $zero, 1b
    nop
    lw    $t1, GP_STATE($t0)
    nop
    EXPECT $t1, 0x0000011f       # 1: frame 1, no fault
    lui   $s1, 0x1040            # frame 1 base
    lw    $t1, 0($s1)            # row 0, x 0: edge
    nop
    EXPECT $t1, 0x00123456       # 2
    lw    $t1, 4($s1)            # row 0, x 1: fill
    nop
    EXPECT $t1, 0xff123456       # 3
    lw    $t1, 0xc7c($s1)        # row 0, x 799: edge
    nop
    EXPECT $t1, 0x00123456       # 4
    li    $t2, 0x10657640        # row 599, x 400: fill
    lw    $t1, 0($t2)
    nop
    EXPECT $t1, 0xff123456       # 5
    li    $t1, 0x00000001        # STOP with a stray bit: fault
    sw    $t1, 0($s0)
    sw    $s0, GP_CODE($t0)
2:  lw    $t1, GP_STATE($t0)
    nop
    andi  $t1, $t1, 1
    beq   $t1, $zero, 2b
    nop
    lw    $t1, GP_STATE($t0)
    nop
    EXPECT $t1, 0x0000811f       # 6: fault (bit 15), frame 1
    break
