# irq_defer.s - when an interrupt may enter (CPUMIPS.v:114 DO_ISR, with tests/hw/isr.s mode 0 at
# 0xC0000180).  DO_ISR replaces the fetch after the instruction in F, so EPC is the next PC; it is
# held off when that instruction is an mtc0 or could branch, and for a taken branch's target.
# With the timer bit already pending: enabling IE by mtc0, the next instruction still runs first;
# before a taken branch, the entry comes after the delay slot (EPC = target); enabling IE in a taken
# branch's delay slot (the cs150 `jr $k0; mtc0 Status` return), the target and one more run first.
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    move  $s0, $zero             # isr.s mode 0: clear the cause
    move  $s1, $zero
    move  $s2, $zero
    move  $s6, $zero
    move  $t7, $zero
    mtc0  $zero, $12
    jal   pend                   # make Cause bit 15 pending (IE off)
    nop
    li    $t1, 0x8001
    mtc0  $t1, $12               # IE on with IP15 pending: no entry right after an mtc0
    addiu $t7, $t7, 1            # runs before the ISR
epc1:
    nop
    move  $t4, $s1
    la    $t2, epc1
    subu  $t4, $t4, $t2
    EXPECT $t4, 0                # EPC = epc1
    EXPECT $t7, 1
    EXPECT $s6, 1
    mtc0  $zero, $12
    jal   pend
    nop
    li    $t1, 0x8001
    mtc0  $t1, $12               # IE on, then a taken branch
    b     tgt
    nop                          # the branch could branch: held off until its delay slot has run
    addiu $t7, $t7, 100          # skipped
tgt:                             # entry after the delay slot: EPC = the branch target
    addiu $t7, $t7, 1
    move  $t4, $s1
    la    $t2, tgt
    subu  $t4, $t4, $t2
    EXPECT $t4, 0                # EPC = tgt
    EXPECT $s6, 2
    mtc0  $zero, $12
    jal   pend
    nop
    li    $t1, 0x8001
    b     tgt2
    mtc0  $t1, $12               # IE on in the delay slot (like the cs150 ISR return)
    addiu $t7, $t7, 100          # skipped
tgt2:
    addiu $t7, $t7, 1            # the target: held off (WAS_Branch)
    addiu $t7, $t7, 1            # entry after this one
epc3:
    nop
    move  $t4, $s1
    la    $t2, epc3
    subu  $t4, $t4, $t2
    EXPECT $t4, 0                # EPC = epc3
    EXPECT $t7, 4
    EXPECT $s6, 3
    mtc0  $zero, $12
    break
pend:
    mfc0  $t1, $9
    nop
    addiu $t1, $t1, 20
    mtc0  $t1, $11
1:  mfc0  $t3, $13
    nop
    andi  $t3, $t3, 0x8000
    beq   $t3, $zero, 1b
    nop
    jr    $ra
    nop
