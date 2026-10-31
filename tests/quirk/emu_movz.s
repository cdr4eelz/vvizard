# quirk emulation: movz (reserved on: ['t2=0']; off: ['t2=7'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $a0,$zero,7                        # 40000000: 24040007
    addiu $t2,$zero,5                        # 40000004: 240a0005
    movz $t2,$a0,$zero                       # 40000008: 0080500a
    break                                    # 4000000c: 0000000d
