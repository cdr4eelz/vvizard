# quirk emulation: cop0 (cop0 on: ['t2=0', 't3=0xffff']; off: ['t2=0x1234'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $t0,$zero,4660                     # 40000000: 24081234
    mtc0 $t0,$14                             # 40000004: 40887000
    mfc0 $t2,$14                             # 40000008: 400a7000
    mfc0 $t3,$11                             # 4000000c: 400b5800
    break                                    # 40000010: 0000000d
