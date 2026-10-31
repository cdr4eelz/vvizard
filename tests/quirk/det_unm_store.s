# quirk detection: unm_store (trips unmapped)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    sw $t1,0($zero)                          # 40000000: ac090000
    break                                    # 40000004: 0000000d
