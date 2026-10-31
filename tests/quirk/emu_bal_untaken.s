# quirk emulation: bal_untaken (bal-untaken on: ['ra=0']; off: ['ra=0x40000010'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $ra,$zero,0                        # 40000000: 241f0000
    addiu $a0,$zero,1                        # 40000004: 24040001
    bltzal $a0,L_40000010                    # 40000008: 04900001
    nop                                      # 4000000c: 00000000
L_40000010:
    break                                    # 40000010: 0000000d
