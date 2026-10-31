# quirk detection: syscall (trips syscall)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    syscall                                  # 40000000: 0000000c
    break                                    # 40000004: 0000000d
