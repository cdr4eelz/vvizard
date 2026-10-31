# quirk emulation: ddr2 (ddr2 on: ['t2=0']; off: ['t2=0x55'])
# vv-load: 0x60000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x2000                           # 60000000: 3c082000
    addiu $t1,$zero,85                       # 60000004: 24090055
    sw $t1,256($t0)                          # 60000008: ad090100
    lui $t3,0x1000                           # 6000000c: 3c0b1000
    lw $t2,256($t3)                          # 60000010: 8d6a0100
    nop                                      # 60000014: 00000000
    break                                    # 60000018: 0000000d
