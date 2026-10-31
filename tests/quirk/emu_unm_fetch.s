# quirk emulation: unm_fetch (unmapped-fetch on: rc=0; off: rc!=0)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x9000                           # 40000000: 3c089000
    jr $t0                                   # 40000004: 01000008
    nop                                      # 40000008: 00000000
    break                                    # 4000000c: 0000000d
