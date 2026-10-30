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

.PHONY: all clean fresh run test test-hex test-player test-quirks

all: $(TARGET)

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

test-quirks: $(TARGET)
	python3 tests/test_quirks.py

test-hex: $(TARGET)
	python3 tests/test_hex.py

# All tests (hex programs + hwprobe, quirk detection/emulation, proj7 player via GIOS)
test: test-hex test-quirks test-player

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

fresh: clean
	$(MAKE) all
