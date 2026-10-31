# gp_inline.s - threaded GPU (--threads=on) when the target frame holds code the guest has run:
# host stores from the worker would not drop Unicorn's translations, so that launch draws inline
# (uc_mem_write) with the same busy time.  Copies `jr $ra; nop` to frame 5, runs it, fills frame 5,
# waits and checks the pixels.  Then frame 6: `jr $ra` at row 0 and in the row padding (byte 3200,
# not drawn); the guest runs row 0 while the worker fills frame 6 with 0 (nop); after the ready edge
# the translations of frame 6 must be gone, so row 0 runs 800 nops before the padding's jr.  --no-random-ddr.
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    lui   $s2, 0x1140            # frame 5 (D$ path 0x1xxxxxxx; fetch decodes the same DDR)
    li    $t1, 0x03e00008        # jr $ra
    sw    $t1, 0($s2)
    sw    $zero, 4($s2)          # nop
    jalr  $s2                    # run it: the frame is now executed DDR
    nop
    lui   $s6, 0x1200
    ori   $s6, $s6, 0x3000
    li    $t1, 0x10000000        # FILL colour 0
    sw    $t1, 0($s6)
    sw    $zero, 4($s6)          # STOP
    sw    $s2, GP_FRAME($t0)
    sw    $s6, GP_CODE($t0)
    lw    $t1, GP_STATE($t0)
    nop
    andi  $t1, $t1, 1
    EXPECT $t1, 0                # busy as with the worker
1:  lw    $t1, GP_STATE($t0)
    nop
    andi  $t1, $t1, 1
    beq   $t1, $zero, 1b
    nop
    lw    $t1, 0($s2)
    nop
    EXPECT $t1, 0                # the fill replaced jr $ra
    lw    $t1, 4096($s2)
    nop
    EXPECT $t1, 0
    lui   $s2, 0x1180            # frame 6
    li    $t1, 0x03e00008        # jr $ra at 0 and at 3200
    sw    $t1, 0($s2)
    sw    $zero, 4($s2)
    sw    $t1, 3200($s2)
    sw    $zero, 3204($s2)
    sw    $s2, GP_FRAME($t0)
    sw    $s6, GP_CODE($t0)      # fill frame 6 with 0 (worker: frame 6 has not run yet)
    jalr  $s2                    # runs frame 6 code while it is being drawn
    nop
2:  lw    $t1, GP_STATE($t0)
    nop
    andi  $t1, $t1, 1
    beq   $t1, $zero, 2b
    nop
    mfc0  $s0, $9
    jalr  $s2                    # 800 nops, then the padding's jr $ra
    nop
    mfc0  $s1, $9
    subu  $t2, $s1, $s0
    INRANGE $t3, $t2, 800, 830
    EXPECT $t3, 1
    break
