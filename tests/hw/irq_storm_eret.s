# irq_storm_eret.s - as irq_storm.s, but the ISR runs eret (a nop on the MIPS150) with the cause
# still pending: reported at the eret.
# vv-load: 0x40000000
# vv-isa: -mips32r2
.equiv MODE, 2
.equiv ENTRIES, 3
.include "hw/hw.inc"
.include "hw/timer.inc"
