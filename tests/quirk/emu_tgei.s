# quirk emulation: tgei (reserved on: ['t2=5']; off: ['t2=7'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $a0,$zero,-1                       # 40000000: 2404ffff
    addiu $t2,$zero,0                        # 40000004: 240a0000
    tgei $a0,2                               # 40000008: 04880002
    addiu $t2,$t2,1                          # 4000000c: 254a0001
    addiu $t2,$t2,2                          # 40000010: 254a0002
    addiu $t2,$t2,4                          # 40000014: 254a0004
    break                                    # 40000018: 0000000d
