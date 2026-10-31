# quirk emulation: jalr_rd (jalr-rd on: ['ra=0x40000014', 't1=0']; off: ['t1=0x40000014', 'ra=0'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x4000                           # 40000000: 3c084000
    ori $t0,$t0,0x14                         # 40000004: 35080014
    addiu $ra,$zero,0                        # 40000008: 241f0000
    jalr $t1,$t0                             # 4000000c: 01004809
    nop                                      # 40000010: 00000000
    break                                    # 40000014: 0000000d
