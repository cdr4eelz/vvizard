# quirk emulation: syscall (syscall on: ['t2=2']; off: exc)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $t2,$zero,1                        # 40000000: 240a0001
    syscall                                  # 40000004: 0000000c
    addiu $t2,$zero,2                        # 40000008: 240a0002
    break                                    # 4000000c: 0000000d
