# hwprobe.s - MIPS150-vs-MIPS32 probe, the evidence behind vvizard's default quirk set.
# vv-load: 0x60000000
# vv-isa: -mips32r2
# Every HCHECK expects the value the RTL produces (run on the sim-icarus rig: CPU_prog_tb +
# CoreHarness, 45 checks, all match except #39 where CoreHarness returns 0xDEADBEEF for an
# unmapped load while MemBank returns 0).  In vvizard (tests/test_hex.py):
#   vvizard --no-bios --no-random-ddr --load hwprobe.bin@0x60000000 --entry 0x60000000 \
#           --expect s5=0 --expect s3=45          # default quirks = current hardware
# Build: make tests (vvizard Makefile: build/tests/hwprobe.bin / .hex; see README "Tests").
# HW-vs-MIPS probe. Each CHECK expects the MIPS150 (RTL) value. $s5 counts mismatches
# (for vvizard: --expect s5=0), $s4 = index of first mismatch + 1.
.set noreorder
.set noat
.include "check.inc"
.macro HCHECK r, exp
    CHECK \r, \exp
    li    $t9, \exp
    lw    $t8, -8($s7)
    nop
    beq   $t8, $t9, 98f
    addiu $s3, $s3, 1
    bne   $s4, $zero, 98f
    nop
    move  $s4, $s3
    addiu $s5, $s5, 1
98:
.endm
.text
.global _start
_start:
    BEGIN_CHECKS
    li    $s3, 0
    li    $s4, 0
    li    $s5, 0
    lui   $s6, 0x5000
    ori   $s6, $s6, 0x0100
    li    $t0, 0x80FF7F01
    sw    $t0, 0($s6)
    li    $t0, 0x11223344
    sw    $t0, 4($s6)
    sw    $zero, 8($s6)
# 1 unaligned loads -> 0
    li    $t0, 0x5555
    lw    $t0, 1($s6); nop;  HCHECK $t0, 0
    li    $t0, 0x5555
    lh    $t0, 1($s6); nop;  HCHECK $t0, 0
    li    $t0, 0x5555
    lhu   $t0, 3($s6); nop;  HCHECK $t0, 0
# 2 unaligned stores dropped
    li    $t1, 0xAAAAAAAA
    sw    $t1, 2($s6)
    sh    $t1, 5($s6)
    lw    $t0, 0($s6); nop;  HCHECK $t0, 0x80FF7F01
    lw    $t0, 4($s6); nop;  HCHECK $t0, 0x11223344
# 3 lwl/lwr -> 0 ; swl/swr dropped
    li    $t0, 0x12345678
    lwl   $t0, 1($s6); nop;  HCHECK $t0, 0
    li    $t0, 0x12345678
    lwr   $t0, 2($s6); nop;  HCHECK $t0, 0
    swl   $t1, 1($s6)
    swr   $t1, 6($s6)
    lw    $t0, 0($s6); nop;  HCHECK $t0, 0x80FF7F01
    lw    $t0, 4($s6); nop;  HCHECK $t0, 0x11223344
# 4 HI/LO
    li    $a0, 3
    li    $a1, 5
    li    $t0, 0x777
    mult  $a0, $a1
    mflo  $t0;               HCHECK $t0, 0
    li    $t0, 0x777
    mthi  $a0
    mfhi  $t0;               HCHECK $t0, 0
    li    $t0, 0x777
    divu  $a1, $a0
    mflo  $t0;               HCHECK $t0, 0
# 5 overflow wraps
    li    $a0, 0x7FFFFFFF
    li    $a1, 1
    add   $t0, $a0, $a1;     HCHECK $t0, 0x80000000
    addi  $t0, $a0, 1;       HCHECK $t0, 0x80000000
    li    $a2, 0x80000000
    sub   $t0, $a2, $a1;     HCHECK $t0, 0x7FFFFFFF
# 6 syscall nop
    li    $t0, 1
    syscall
    addiu $t0, $t0, 1;       HCHECK $t0, 2
# 7 movz/movn -> rd = 0
    li    $t0, 0x1234
    movz  $t0, $a0, $zero;   HCHECK $t0, 0
    li    $t2, 0x99
    movn  $t2, $a0, $a0;     HCHECK $t2, 0
# 8 branch-likely = nop
    li    $t0, 0
    beql  $zero, $zero, 1f
    addiu $t0, $t0, 1
    addiu $t0, $t0, 2
1:  addiu $t0, $t0, 4;       HCHECK $t0, 7
    li    $t0, 0
    bnel  $zero, $zero, 2f
    addiu $t0, $t0, 1
    addiu $t0, $t0, 2
2:  nop;                     HCHECK $t0, 3
# 9 REGIMM likely = non-likely; untaken bltzall: no link
    li    $a0, 1
    li    $t0, 0
    bltzl $a0, 3f
    addiu $t0, $t0, 1
    addiu $t0, $t0, 2
3:  nop;                     HCHECK $t0, 3
    li    $ra, 0
    bltzall $a0, 4f
    nop
4:  nop;                     HCHECK $ra, 0
# 10 tgei acts as bltz (HW branches when rs<0)
    li    $a0, -1
    li    $t0, 0
    .word 0x04880002          # tgei $a0, 2  (HW: bltz $a0, +2 insns)
    addiu $t0, $t0, 1
    addiu $t0, $t0, 2
    addiu $t0, $t0, 4;       HCHECK $t0, 5
# 11 load-use: next insn sees OLD value; delay-slot write wins
    li    $t0, 0x1111
    lw    $t0, 0($s6)
    addu  $t1, $t0, $zero
    nop;                     HCHECK $t1, 0x1111
                             HCHECK $t0, 0x80FF7F01
    li    $t0, 0x10
    lw    $t0, 0($s6)
    addiu $t0, $t0, 1
    nop;                     HCHECK $t0, 0x11
# 12 rotr -> srl
    li    $a0, 0x80000001
    rotr  $t0, $a0, 1;       HCHECK $t0, 0x40000000
    li    $a1, 4
    rotrv $t0, $a0, $a1;     HCHECK $t0, 0x08000000
# 13 COP0150
    li    $a0, 0x1234
    mtc0  $a0, $14
    mfc0  $t0, $14;          HCHECK $t0, 0
    mfc0  $t0, $15;          HCHECK $t0, 0
    mfc0  $t0, $11;          HCHECK $t0, 0xFFFF
    li    $a0, 0xFFFF0000
    mtc0  $a0, $12
    mfc0  $t0, $12;          HCHECK $t0, 0xFFFF0000
    mtc0  $zero, $12
    mfc0  $t0, $12, 1;       HCHECK $t0, 0
# 14 eret / clz / mul / ll / sc = nop
    li    $t0, 0x55
    eret
    addiu $t0, $t0, 1;       HCHECK $t0, 0x56
    li    $t0, 0x55
    clz   $t0, $a0;          HCHECK $t0, 0x55
    li    $t0, 0x55
    mul   $t0, $a0, $a0;     HCHECK $t0, 0x55
    li    $t0, 0x55
    ll    $t0, 0($s6); nop;  HCHECK $t0, 0x55
    li    $t0, 0x55
    sc    $t0, 8($s6)
    lw    $t1, 8($s6); nop;  HCHECK $t1, 0
                             HCHECK $t0, 0x55
# 15 ops 0x27/0x2C/0x2D/0x2F decode as lw/sb/sh/sw but have no ALU op (ALUdec default
#    -> ALU_XXX = 0), so they access address 0 (undecoded in MemBank: load 0, store dropped).
#    (CoreHarness returns 0xDEADBEEF for unmapped loads, so check #39 fails there by design.)
    li    $t0, 0x55
    .word 0x9EC80000          # op 0x27 rt=t0 rs=s6: HW lw from 0x00000000 -> 0
    nop;                     HCHECK $t0, 0
    li    $t1, 0xCAFE
    .word 0xBEC90008          # op 0x2F (cache) rt=t1 rs=s6 +8: HW sw to 0x00000000 (dropped)
    lw    $t0, 8($s6); nop;  HCHECK $t0, 0
# 16 lb/lh sign-extend (fixed)
    lb    $t0, 0($s6); nop;  HCHECK $t0, 0xFFFFFF80
    lh    $t0, 0($s6); nop;  HCHECK $t0, 0xFFFF80FF
# 17 jalr rd (fixed)
    li    $t6, 0
    LA_CODE $t4, 5f
    jalr  $t6, $t4
    nop
5:  nop
    LA_CODE $t5, 5b
    subu  $t6, $t6, $t5;     HCHECK $t6, 0
# 18 untaken bltzal/bgezal: no link
    li    $ra, 0
    li    $a0, 1
    bltzal $a0, 6f
    nop
6:  nop;                     HCHECK $ra, 0
    break                    # vvizard stops here; HW: nop
    END_CHECKS
