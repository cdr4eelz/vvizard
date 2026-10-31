# quirk detection: unm_load (trips unmapped)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x7000                           # 40000000: 3c087000
    lw $t1,0($t0)                            # 40000004: 8d090000
    nop                                      # 40000008: 00000000
    break                                    # 4000000c: 0000000d
