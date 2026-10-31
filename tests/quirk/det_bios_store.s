# quirk detection: bios_store (trips bios-store)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x4000                           # 40000000: 3c084000
    sw $t0,256($t0)                          # 40000004: ad080100
    break                                    # 40000008: 0000000d
