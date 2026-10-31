# isr.s - interrupt handler for the tests/hw timer programs, loaded at 0xC0000180 (PC_ISR).
# Counts entries in $s6, ORs Cause into $s2, saves EPC in $s1.  $s0 = mode: 0 clears the cause on
# every entry; 1 and 2 leave it pending on the first two entries (2 also runs eret, a nop on the
# MIPS150).  Returns like cs150 isr7.s: jr EPC with mtc0 Status (IE=1) in the delay slot.
# vv-load: 0xC0000180
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    mfc0  $k1, $13
    nop
    or    $s2, $s2, $k1
    mfc0  $s1, $14
    addiu $s6, $s6, 1
    beq   $s0, $zero, clear
    nop
    sltiu $k0, $s6, 3
    beq   $k0, $zero, clear      # third entry: clean up
    nop
    li    $k0, 2
    bne   $s0, $k0, ret
    nop
    eret                         # MIPS150: nop
    b     ret
    nop
clear:
    li    $k0, -1
    mtc0  $k0, $11               # Compare write clears IP15 (next match ~86 s away)
    mtc0  $zero, $13             # clear all Cause IP bits
ret:
    mfc0  $k1, $12
    mfc0  $k0, $14
    ori   $k1, $k1, 1
    jr    $k0
    mtc0  $k1, $12
