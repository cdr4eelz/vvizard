# sm_magic2.s - a proj7 shared memory (shared7.h) at 0x50000000 whose magic2 (0x13C) was never
# written: magic1 0xFEDBEEF3, majorvers 7, buff_size 256, player x 12 / y 34, bouncers 5/9.
# tests/test_sm.py expects the proj7 decode plus a "magic2 ... != magic1" warning at exit.
# vv-load: 0x40000000
# vv-isa: -mips1
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    lui   $t1, 0x5000
    li    $t2, 0xFEDBEEF3
    sw    $t2, 0x00($t1)         # magic1
    li    $t2, 7
    sw    $t2, 0x04($t1)         # majorvers
    li    $t2, 34
    sh    $t2, 0x28($t1)         # player_y
    li    $t2, 12
    sh    $t2, 0x2A($t1)         # player_x
    li    $t2, 9
    sh    $t2, 0x2C($t1)         # max_bouncers
    li    $t2, 5
    sh    $t2, 0x2E($t1)         # num_bouncers
    li    $t2, 256
    sw    $t2, 0x30($t1)         # buff_size
    break
