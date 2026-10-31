# quirk emulation: mmio_lane (mmio-lane on: b'\x00'; off: b'A')
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x8000                           # 40000000: 3c088000
    addiu $t1,$zero,65                       # 40000004: 24090041
    sb $t1,8($t0)                            # 40000008: a1090008
    break                                    # 4000000c: 0000000d
