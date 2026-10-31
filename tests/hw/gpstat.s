# gpstat.s - GP_STATE (0x8000005C) = {pf_status, gp_status} in the graphics.h gstate_s layout:
# pf_frame 29:24; gp_fault 15, gp_procframe 13:8, engines ready 4:0.  (GPU.v:96-99 as written is
# 17 bits wide and drops the fault bit; vvizard models the intended layout: QUIRKS.md "gp-status".)
# A launch latches the frame, sets fault on a bad opcode, and raises irq_gp_done (Cause bit 12).
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    mtc0  $zero, $13
    lw    $t1, GP_STATE($t0)
    nop
    EXPECT $t1, 0x0000001f       # frame 0, no fault, all engines ready
    lui   $s0, 0x1200
    ori   $s0, $s0, 0x3000       # command list in DDR (graphics.h GPTEMP)
    li    $t1, 0xe0000000        # opcode 14: not a GP command -> fault
    sw    $t1, 0($s0)
    sw    $zero, 4($s0)
    li    $t1, 0x11400000        # frame 5
    sw    $t1, GP_FRAME($t0)
    sw    $s0, GP_CODE($t0)      # launch
    lw    $t1, GP_STATE($t0)
    nop
    EXPECT $t1, 0x0000851f       # fault=1 (bit 15), frame 5 at 13:8
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x1000
    EXPECT $t3, 0x1000           # irq_gp_done
    sw    $zero, 0($s0)          # STOP only
    li    $t1, 0x10c00000        # frame 3
    sw    $t1, GP_FRAME($t0)
    sw    $s0, GP_CODE($t0)
    lw    $t1, GP_STATE($t0)
    nop
    EXPECT $t1, 0x0000031f       # launch clears the fault
    li    $t1, 0x10800000        # PF frame 2 -> pf_frame field after the next frame end
    sw    $t1, PF_FRAME($t0)
1:  lw    $t1, PF_FRAME($t0)
    nop
    srl   $t1, $t1, 22
    andi  $t1, $t1, 0x3f
    li    $t9, 2
    bne   $t1, $t9, 1b
    nop
    lw    $t1, GP_STATE($t0)
    nop
    EXPECT $t1, 0x0200031f
    break
