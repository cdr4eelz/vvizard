# quirk emulation: overflow (overflow on: ['t2=0x80000000', 'a0=0x7fffffff', 'a1=1']; off: exc)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $a0,0x7fff                           # 40000000: 3c047fff
    ori $a0,$a0,0xffff                       # 40000004: 3484ffff
    addiu $a1,$zero,1                        # 40000008: 24050001
    add $t2,$a0,$a1                          # 4000000c: 00855020
    break                                    # 40000010: 0000000d
