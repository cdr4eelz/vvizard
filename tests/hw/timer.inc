# timer.inc - body of timer.s, irq_storm.s and irq_storm_eret.s (MODE, ENTRIES set by the includer).
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    li    $s0, MODE
    move  $s1, $zero
    move  $s2, $zero
    move  $s6, $zero
    mtc0  $zero, $12
    mfc0  $t1, $9
    nop
    addiu $t1, $t1, 1000
    mtc0  $t1, $11               # Compare = Count + 1000 (also clears IP15)
    mtc0  $zero, $13
1:  mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x8000
    beq   $t3, $zero, 1b
    nop
    mfc0  $t2, $9
    nop
    subu  $t2, $t2, $t1
    INRANGE $t4, $t2, 0, 12
    EXPECT $t4, 1                # fired at Count == Compare
    mtc0  $t1, $11               # Compare write clears IP15
    mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x8000
    EXPECT $t3, 0
    # interrupt entry
    mfc0  $t1, $9
    nop
    addiu $t1, $t1, 500
    mtc0  $t1, $11
    li    $t1, 0x8001            # IM15 | IE
    mtc0  $t1, $12
    li    $t5, 100000
2:  bne   $s6, $zero, 3f
    nop
    addiu $t5, $t5, -1
    bne   $t5, $zero, 2b
    nop
3:  li    $t5, 3000              # let a storm play out
4:  addiu $t5, $t5, -1
    bne   $t5, $zero, 4b
    nop
    mtc0  $zero, $12
    andi  $t2, $s2, 0x8000
    EXPECT $t2, 0x8000           # the ISR saw Cause bit 15
    INRANGE $t6, $s1, 0x40000000, 0x40001000
    EXPECT $t6, 1                # EPC in this program
    EXPECT $s6, ENTRIES          # ISR entries
    break
