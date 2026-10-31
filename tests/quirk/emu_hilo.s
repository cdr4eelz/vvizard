# quirk emulation: hilo (hilo on: ['t2=0']; off: ['t2=15'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $a0,$zero,3                        # 40000000: 24040003
    addiu $a1,$zero,5                        # 40000004: 24050005
    mult $a0,$a1                             # 40000008: 00850018
    mflo $t2                                 # 4000000c: 00005012
    break                                    # 40000010: 0000000d
