# quirk detection: bal_untaken (trips bal-untaken)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $a0,$zero,1                        # 40000000: 24040001
    bltzal $a0,L_4000000c                    # 40000004: 04900001
    nop                                      # 40000008: 00000000
L_4000000c:
    break                                    # 4000000c: 0000000d
