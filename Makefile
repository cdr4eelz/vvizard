# Prefer a locally built Unicorn (2.1.1+, needs the virtual-TLB API) in ~/.local
UNICORN_PC := $(HOME)/.local/lib/pkgconfig
export PKG_CONFIG_PATH := $(UNICORN_PC)$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))

# VVIZARD: Unicorn-based MIPS emulator for the cs150 project
# Usage: make [all|clean|fresh|run BIN=path/to/prog.elf [ARGS=...]]

TARGET     := vvizard
SRC_DIR    := src
BUILD_DIR  := build

CC         := gcc
CPPFLAGS   ?= -I$(SRC_DIR)
CFLAGS     ?= -std=gnu99 -Wall -Wextra -O0 -g3 -fno-omit-frame-pointer

UNICORN_CFLAGS := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --cflags unicorn)
UNICORN_LIBS   := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs unicorn) -Wl,-rpath,$(HOME)/.local/lib

RAYLIB_CFLAGS  := $(shell pkg-config --cflags raylib)
RAYLIB_LIBS    := $(shell pkg-config --libs raylib)

SRCS := $(wildcard $(SRC_DIR)/*.c)
HDRS := $(wildcard $(SRC_DIR)/*.h)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

BIOS ?= $(abspath ../../software/gios/gios.inst.bin)

SW   := $(abspath ../../software)
# Raw-byte images (no ELF parsing). GIOS is the BIOS at 0x40000000 (BIOS var above).
# IMEM_APP -> I-scratch 0x60000000 (bench alternative: IMEM_APP=$(SW)/bench/bench.inst.bin)
# DMEM_APP -> D-scratch 0x50000000;  ISR_BIN -> ISR BRAM 0xC0000000
IMEM_APP ?= $(SW)/proj_app7/proj7.inst.bin
DMEM_APP ?= $(SW)/draw/draw.inst.bin
ISR_BIN  ?= $(SW)/proj_app7/isr7.bin
# Default: boot GIOS (AUTORUN types "jal 60000000" at its prompt), random DDR, no limit, window on.
BIN  ?= --load $(DMEM_APP)@0x50000000 --load $(ISR_BIN)@0xC0000000 --load $(IMEM_APP)@0x60000000 --entry 0x40000000
# Auto-type the jump to the IMEM app at the GIOS prompt (GIOS ends tokens on space/CR).
AUTORUN ?= jal 60000000\r
ARGS ?= --random-ddr --irq --max-insns 0
# MIPS150 hardware quirks to EMULATE (README "Hardware quirks"; ./vvizard --list-quirks).
# 'default' = the current hardware (fixed bugs jalr-rd/lb-sign off, bal-untaken on).
# e.g. HW_QUIRKS=none (standard MIPS32), HW_QUIRKS=-load-use, HW_QUIRKS=+jalr-rd,+lb-sign (old HW).
# The VVIZARD_QUIRKS environment variable is applied first; HW_QUIRKS overrides it.
HW_QUIRKS ?= default
# Stop with a diagnostic when the guest hits one of the emulated quirks.
# QUIRKS= disables it; e.g. QUIRKS=--halt-on-quirk=-load-use, QUIRKS=--halt-on-quirk=all
# (also non-emulated ones) or QUIRKS=--warn-on-quirk.
QUIRKS ?= --halt-on-quirk

.PHONY: all clean fresh run test test-hex test-player test-quirks tests check-hex refresh-hex

all: $(TARGET)

# ---- Test programs: built from assembly (tests/*.s, tests/quirk/*.s) with the MIPS GNU toolchain ----
# Settings follow cs150 software/makefile.in ASOPTS (+ the -G0 static link of makefile.isr): big-endian
# o32, -non_shared (no abicalls/PIC), no .pdr, --no-break/--no-trap.  Each .s names its load (= link) address and ISA:
#   # vv-load: 0x40000000      # vv-isa: -mips1   (hwprobe and the quirk snippets use -mips32r2)
# Output: build/tests/NAME.bin (raw big-endian .text) and NAME.hex (one word per line).  The tests use
# the built .bin; without a toolchain they fall back to the committed tests/NAME.hex copies
# (make refresh-hex updates those; make check-hex shows any difference).
M_PRE   ?= $(shell command -v mips-linux-gnu-as >/dev/null 2>&1 && echo mips-linux-gnu || \
                   (command -v mips64-linux-gnu-as >/dev/null 2>&1 && echo mips64-linux-gnu))
T_AS    := $(M_PRE)-as
T_LD    := $(M_PRE)-ld
T_COPY  := $(M_PRE)-objcopy
T_ASOPTS := -EB -mabi=32 -non_shared -mno-shared -G0 -mno-pdr --no-break --no-trap --no-pad-sections -Itests
T_SRCS  := $(filter-out tests/check.inc,$(wildcard tests/*.s tests/quirk/*.s))
T_BINS  := $(patsubst tests/%.s,$(BUILD_DIR)/tests/%.bin,$(T_SRCS))
T_HEXS  := $(T_BINS:.bin=.hex)
vv_field = $(shell sed -n 's/^\# vv-$(1): *//p' $(2) | head -1)

ifneq ($(M_PRE),)
tests: $(T_BINS) $(T_HEXS)
else
tests:
	@echo "tests: no mips-linux-gnu toolchain; the tests use the committed tests/*.hex fallbacks"
endif

$(BUILD_DIR)/tests/%.o: tests/%.s tests/check.inc
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	@$(T_AS) $(T_ASOPTS) $(call vv_field,isa,$<) $< -o $@

$(BUILD_DIR)/tests/%.elf: $(BUILD_DIR)/tests/%.o tests/test.ld
	@$(T_LD) -EB -static -nostdlib --build-id=none --no-warn-rwx-segments \
	    --defsym=VV_LOAD=$(call vv_field,load,tests/$*.s) -T tests/test.ld $< -o $@

$(BUILD_DIR)/tests/%.bin: $(BUILD_DIR)/tests/%.elf
	@$(T_COPY) -O binary -j .text $< $@

$(BUILD_DIR)/tests/%.hex: $(BUILD_DIR)/tests/%.bin
	@od -An -v -tx1 $< | tr -d ' \n' | fold -w8 > $@; echo >> $@

# Compare the built programs with the committed fallback hex (byte for byte).
.SECONDARY:

ifneq ($(M_PRE),)
check-hex: tests
	@bad=0; for h in $(T_HEXS); do c=tests/$${h#$(BUILD_DIR)/tests/}; \
	  if [ ! -f $$c ]; then echo "check-hex: $$c missing (make refresh-hex)"; bad=1; \
	  elif [ "$$(tr -d ' \n' < $$c)" != "$$(tr -d ' \n' < $$h)" ]; then echo "check-hex: $$c differs from $$h"; bad=1; fi; done; \
	[ $$bad = 0 ] && echo "check-hex: all $(words $(T_HEXS)) programs match their committed hex"
else
check-hex:
	@echo "check-hex: skipped (no MIPS toolchain)"
endif

refresh-hex: tests
	@for h in $(T_HEXS); do cp $$h tests/$${h#$(BUILD_DIR)/tests/}; done; echo "refresh-hex: $(words $(T_HEXS)) files"

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(UNICORN_LIBS) $(RAYLIB_LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HDRS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(UNICORN_CFLAGS) $(RAYLIB_CFLAGS) $(CFLAGS) -c $< -o $@

# Default BIOS path compiled into main.c (override at run time with --bios / --no-bios)
$(BUILD_DIR)/main.o: CPPFLAGS += -DVV_DEFAULT_BIOS='"$(BIOS)"'

$(BUILD_DIR):
	mkdir -p $@

run: $(TARGET)
	LD_LIBRARY_PATH=$(HOME)/.local/lib ./$(TARGET) --bios $(BIOS) $(if $(AUTORUN),--uart-in '$(AUTORUN)') --quirks=$(HW_QUIRKS) $(QUIRKS) $(ARGS) $(BIN)

test-player: $(TARGET)
	python3 tests/test_player.py

test-quirks: $(TARGET) tests
	python3 tests/test_quirks.py

test-hex: $(TARGET) tests
	python3 tests/test_hex.py

# All tests (hex programs + hwprobe, quirk detection/emulation, proj7 player via GIOS)
test: check-hex test-hex test-quirks test-player

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

fresh: clean
	$(MAKE) all
