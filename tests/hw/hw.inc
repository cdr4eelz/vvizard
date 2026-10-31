# hw.inc - macros for the hardware-model tests (tests/hw/*.s, run by tests/test_hw.py).
# $s3 counts EXPECTs, $s5 counts mismatches, $s4 = number of the first mismatching EXPECT.
# The harness checks --expect s5=0 --expect s3=N.  Uses $t9.  Every program starts with HW_INIT.
.equiv IO,       0x8000          # lui base of the MMIO window (0x80000000)
.equiv TX_READY, 0x00
.equiv RX_VALID, 0x04
.equiv TX_DATA,  0x08
.equiv RX_DATA,  0x0c
.equiv PF_FRAME, 0x50
.equiv GP_FRAME, 0x54
.equiv GP_CODE,  0x58
.equiv GP_STATE, 0x5c
.equiv RANDOM32, 0x80
.equiv VV_EXIT,  0x84            # guest exit register (vvizard only; extended area 0x84-0xFC)

.macro HW_INIT                   # registers are not preset (as on hardware): write before reading
    move  $s3, $zero
    move  $s4, $zero
    move  $s5, $zero
    lui   $t0, IO
.endm

.macro EXPECT r, exp
    li    $t9, \exp
    addiu $s3, $s3, 1
    beq   \r, $t9, 101f
    nop
    addiu $s5, $s5, 1
    bne   $s4, $zero, 101f
    nop
    move  $s4, $s3
101:
.endm

# \dst = 1 if lo <= \r <= hi else 0 (unsigned)
.macro INRANGE dst, r, lo, hi
    li    $t9, \lo
    sltu  \dst, \r, $t9
    xori  \dst, \dst, 1
    li    $t9, \hi + 1
    sltu  $t9, \r, $t9
    and   \dst, \dst, $t9
.endm
