# quirk detection: hilo (trips hilo)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $t0,$zero,3                        # 40000000: 24080003
    mult $t0,$t0                             # 40000004: 01080018
    mflo $t1                                 # 40000008: 00004812
    break                                    # 4000000c: 0000000d
