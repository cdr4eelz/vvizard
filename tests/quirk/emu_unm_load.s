# quirk emulation: unm_load (unmapped on: ['t1=0', 't2=6']; off: rc!=0)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x7000                           # 40000000: 3c087000
    addiu $t1,$zero,5                        # 40000004: 24090005
    addiu $t2,$zero,6                        # 40000008: 240a0006
    sw $t2,0($t0)                            # 4000000c: ad0a0000
    lw $t1,0($t0)                            # 40000010: 8d090000
    nop                                      # 40000014: 00000000
    break                                    # 40000018: 0000000d
