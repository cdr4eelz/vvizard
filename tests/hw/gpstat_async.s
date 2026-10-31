# gpstat_async.s - GP_STATE with the threaded GPU (--threads=on; docs/MULTITHREADING-PLAN.md section 6).
# A launch clears the engine ready bits at once; they rise (and Cause bit 12 is latched) at
# launch + pixels/4 + 32 x commands cycles.  A GP_CODE store while busy is dropped (gp-busy).
# FILL + STOP: 480000 pixels / 4 + 2 x 32 = 120064 cycles.  Run with --no-random-ddr.
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
    EXPECT $t1, 0x0000001f       # idle: all engines ready
    lui   $s0, 0x1200
    ori   $s0, $s0, 0x3000       # command list in DDR (graphics.h GPTEMP)
    li    $t1, 0xe0000000        # bad opcode -> fault, 1 command: busy 32 cycles
    sw    $t1, 0($s0)
    li    $t1, 0x11400000        # frame 5
    sw    $t1, GP_FRAME($t0)
    sw    $s0, GP_CODE($t0)      # launch
    lw    $t1, GP_STATE($t0)
    nop
    EXPECT $t1, 0x00000500       # busy: ready bits 0, frame 5, fault not yet
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x1000
    EXPECT $t3, 0                # no irq_gp_done yet
1:  lw    $t1, GP_STATE($t0)     # GP_WAIT()
    nop
    andi  $t2, $t1, 1
    beq   $t2, $zero, 1b
    nop
    EXPECT $t1, 0x0000851f       # done: fault, frame 5, ready
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x1000
    EXPECT $t3, 0x1000           # irq_gp_done at the ready edge
    mtc0  $zero, $13
    li    $t1, 0x10123456        # FILL 0x123456
    sw    $t1, 0($s0)
    sw    $zero, 4($s0)          # STOP
    li    $t1, 0x10c00000        # frame 3
    sw    $t1, GP_FRAME($t0)
    mfc0  $s1, $9                # Count before the launch
    sw    $s0, GP_CODE($t0)      # launch (fill)
    li    $t1, 0x11000000        # frame 4
    sw    $t1, GP_FRAME($t0)
    sw    $s0, GP_CODE($t0)      # busy: dropped (gp-busy guest bug)
2:  lw    $t1, GP_STATE($t0)
    nop
    andi  $t2, $t1, 1
    beq   $t2, $zero, 2b
    nop
    mfc0  $s2, $9                # Count after
    EXPECT $t1, 0x0000031f       # frame 3 (the dropped launch changed nothing), no fault
    subu  $t2, $s2, $s1
    INRANGE $t3, $t2, 120064, 120200
    EXPECT $t3, 1                # busy for pixels/4 + 32 x commands
    lui   $t4, 0x10c0            # frame 3, pixel (0,0) and (799,599)
    lw    $t1, 0($t4)
    nop
    EXPECT $t1, 0x00123456
    li    $t5, 599 * 4096 + 799 * 4
    addu  $t4, $t4, $t5
    lw    $t1, 0($t4)
    nop
    EXPECT $t1, 0x00123456
    lui   $t4, 0x1100            # frame 4 untouched (DDR starts 0)
    lw    $t1, 0($t4)
    nop
    EXPECT $t1, 0
    break
