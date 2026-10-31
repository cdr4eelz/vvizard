# quirk detection: mmio_fetch (trips unmapped-fetch)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x8000                           # 40000000: 3c088000
    jr $t0                                   # 40000004: 01000008
    nop                                      # 40000008: 00000000
    break                                    # 4000000c: 0000000d
