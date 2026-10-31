# timer.s - COP0150 timer: Cause bit 15 when Count == Compare; a Compare write clears it.
# Then the interrupt entry (CPUMIPS.v DO_ISR): Status.IE && (IM & IP) jumps to 0xC0000180 with
# EPC = the interrupted pc and IE = 0.  tests/hw/isr.s (loaded at 0xC0000180) counts entries in
# $s6, ORs the Cause it saw into $s2, saves EPC in $s1; $s0 selects how it returns (0 = clears the
# cause, 1 = leaves it pending for two entries (irq-storm), 2 = same, but returns via eret first).
# vv-load: 0x40000000
# vv-isa: -mips32r2
.equiv MODE, 0
.equiv ENTRIES, 1
.include "hw/hw.inc"
.include "hw/timer.inc"
