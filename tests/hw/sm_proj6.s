# sm_proj6.s - a proj6-layout shared memory (software/proj_app6/shared6.h) at 0x50000000 for
# tests/test_sm.py --dump-sm: magic 0xFEDBEEF2, flags 0x0001 (pause) at 0x16, clock 0x0123 at 0x20,
# player_y -5 / player_x 77 at 0x24 / 0x26, buff_size 256 at 0x28, head 3 / tail 1.
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
    li    $t2, 0xFEDBEEF2
    sw    $t2, 0x00($t1)         # magic
    li    $t2, 1
    sh    $t2, 0x16($t1)         # flags: pause
    li    $t2, 0x0123
    sw    $t2, 0x20($t1)         # clock 01:23 (BCD)
    li    $t2, -5
    sh    $t2, 0x24($t1)         # player_y
    li    $t2, 77
    sh    $t2, 0x26($t1)         # player_x
    li    $t2, 256
    sw    $t2, 0x28($t1)         # buff_size
    li    $t2, 3
    sw    $t2, 0x2C($t1)         # buff_head
    li    $t2, 1
    sw    $t2, 0x30($t1)         # buff_tail
    break
