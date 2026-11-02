# sm_entry.s - the isr7.s utility jump table at 0xC0000000 (16-byte slots: slot 0 DO_INIT_VERSION,
# 1 DO_DISABLE_GLOBAL, 2 DO_DISABLE_ALL, 3-9 DISPATCH with $a0 = slot number; unknown codes return).
# Run by tests/test_sm.py with the real software/proj_app7/isr7.bin loaded at 0xC0000000:
#   slot 0 stamps the proj7 shared memory at 0x50000000 with shared7.s.inc's K_MAGIC_VERSION /
#          K_MAJOR_VERSION (magic1, magic2, majorvers, buff_size) -> checked with --dump-sm
#   slot 1 with Status 0x0C01: Status & IM_POSSIBLE = 0x0C00 (global IE cleared, IM kept)   EXPECT 1
#   slot 2: Status = 0                                                                      EXPECT 2
#   slot 3: DISPATCH code 3 (unknown) returns through $ra                                  EXPECT 3
# vv-load: 0x40000000
# vv-isa: -mips1
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    lui   $s6, 0xC000            # isr7 _entry (jump table)
    jalr  $s6                    # slot 0: DO_INIT_VERSION (clobbers $t0, $t1)
    nop
    li    $t2, 0x0C01            # IM UARX|UATX + IE (no UART traffic: nothing latches)
    mtc0  $t2, $12
    nop
    addiu $t3, $s6, 0x10
    jalr  $t3                    # slot 1: DO_DISABLE_GLOBAL
    nop
    mfc0  $s0, $12
    nop
    EXPECT $s0, 0x0C00           # 1
    addiu $t3, $s6, 0x20
    jalr  $t3                    # slot 2: DO_DISABLE_ALL
    nop
    mfc0  $s1, $12
    nop
    EXPECT $s1, 0                # 2
    move  $s2, $zero
    addiu $t3, $s6, 0x30
    jalr  $t3                    # slot 3: DISPATCH, $a0 = 3 (unknown): jr $ra
    nop
    li    $s2, 1
    EXPECT $s2, 1                # 3
    break
