# pf.s - PixelFeeder (PixelFeeder.v): a PF_FRAME store only latches frame_next; PF_FRAME reads
# return the frame being scanned, which switches at the end of the frame being fetched
# (865800 cycles = 1040x666 @ 40 MHz in 50 MHz cycles), when irq_frame sets Cause bit 13.
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    lw    $t1, PF_FRAME($t0)
    nop
    EXPECT $t1, 0x10000000       # frame 0 at reset
    li    $t1, 0x10400000        # frame 1
    sw    $t1, PF_FRAME($t0)
    lw    $t2, PF_FRAME($t0)
    nop
    EXPECT $t2, 0x10000000       # not switched yet
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x2000
    EXPECT $t3, 0
1:  mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x2000
    beq   $t3, $zero, 1b
    nop
    mfc0  $s0, $9                # Count at the frame interrupt
    lw    $t2, PF_FRAME($t0)
    nop
    EXPECT $t2, 0x10400000       # switched with the interrupt
    INRANGE $t4, $s0, 865800, 865820
    EXPECT $t4, 1
    mtc0  $zero, $13
    li    $t1, 0x10800000        # frame 2: takes effect one frame later
    sw    $t1, PF_FRAME($t0)
2:  lw    $t2, PF_FRAME($t0)
    nop
    srl   $t2, $t2, 22
    andi  $t2, $t2, 0x3f
    li    $t9, 2
    bne   $t2, $t9, 2b
    nop
    mfc0  $s1, $9
    nop
    INRANGE $t4, $s1, 1731600, 1731620
    EXPECT $t4, 1
    break
