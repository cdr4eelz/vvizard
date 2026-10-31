# Prefer a locally built Unicorn (2.1.1+, needs the virtual-TLB API) in ~/.local
UNICORN_PC := $(HOME)/.local/lib/pkgconfig
export PKG_CONFIG_PATH := $(UNICORN_PC)$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))

# VVIZARD: Unicorn-based MIPS emulator for the cs150 project
# Usage: make [all|clean|fresh|test|run|run-<variant>] [DEBUG=1] [EXTRA='vvizard args']

TARGET     := vvizard
SRC_DIR    := src
BUILD_DIR  := build

CC         := gcc
CPPFLAGS   ?= -I$(SRC_DIR)
# Host (vvizard) build: optimized by default; make DEBUG=1 builds the old unoptimized debug flavour.
# (The MIPS test programs use T_ASOPTS below, unaffected.)  Objects rebuild when the flags change.
DEBUG      ?= 0
ifeq ($(DEBUG),1)
OPTFLAGS   ?= -O0 -g3 -fno-omit-frame-pointer
else
OPTFLAGS   ?= -O3 -g
endif
CFLAGS     ?= -std=gnu99 -Wall -Wextra $(OPTFLAGS)
# pthreads (docs/MULTITHREADING-PLAN.md): always linked; --sync / --threads=1 just starts none.
# make TSAN=1: ThreadSanitizer build (Linux; compile and link).
TSAN       ?= 0
THREAD_FLAGS := -pthread$(if $(filter 1,$(TSAN)), -fsanitize=thread)

UNICORN_CFLAGS := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --cflags unicorn)
UNICORN_LIBS   := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs unicorn) -Wl,-rpath,$(HOME)/.local/lib

RAYLIB_CFLAGS  := $(shell pkg-config --cflags raylib)
RAYLIB_LIBS    := $(shell pkg-config --libs raylib)

SRCS := $(wildcard $(SRC_DIR)/*.c)
HDRS := $(wildcard $(SRC_DIR)/*.h)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

# Run settings are vvizard's own command-line arguments (./vvizard --help); with no arguments it
# runs the proj7 player from <vvizard dir>/../../software.  The run-* targets below are canned
# alternatives; EXTRA= appends more arguments to any of them, e.g. make run EXTRA=--trace=100
EXTRA ?=
VV_RUN = LD_LIBRARY_PATH=$(HOME)/.local/lib ./$(TARGET)

.PHONY: all clean fresh run run-std run-legacy-hw run-debug run-warn run-noirq run-gios FORCE test test-hex test-hw test-player test-quirks tests check-hex refresh-hex

all: $(TARGET)

# ---- Test programs: built from assembly (tests/*.s, tests/quirk/*.s, tests/hw/*.s) with the MIPS GNU toolchain ----
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
T_SRCS  := $(wildcard tests/*.s tests/quirk/*.s tests/hw/*.s)
T_BINS  := $(patsubst tests/%.s,$(BUILD_DIR)/tests/%.bin,$(T_SRCS))
T_HEXS  := $(T_BINS:.bin=.hex)
vv_field = $(shell sed -n 's/^\# vv-$(1): *//p' $(2) | head -1)

ifneq ($(M_PRE),)
tests: $(T_BINS) $(T_HEXS)
else
tests:
	@echo "tests: no mips-linux-gnu toolchain; the tests use the committed tests/*.hex fallbacks"
endif

$(BUILD_DIR)/tests/%.o: tests/%.s tests/check.inc $(wildcard tests/hw/*.inc)
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

# Flags stamp: rewritten only when the compile/link command changes, so switching DEBUG/CFLAGS rebuilds everything.
FLAGS_STAMP := $(BUILD_DIR)/.flags
FLAGS_LINE   = $(CC) $(CPPFLAGS) $(UNICORN_CFLAGS) $(RAYLIB_CFLAGS) $(CFLAGS) $(THREAD_FLAGS) LINK=$(LDFLAGS) $(UNICORN_LIBS) $(RAYLIB_LIBS)
$(FLAGS_STAMP): FORCE | $(BUILD_DIR)
	@echo '$(subst ','\'',$(FLAGS_LINE))' | cmp -s - $@ || echo '$(subst ','\'',$(FLAGS_LINE))' > $@

$(TARGET): $(OBJS) $(FLAGS_STAMP)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(LDFLAGS) $(OBJS) -o $@ $(UNICORN_LIBS) $(RAYLIB_LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HDRS) $(FLAGS_STAMP) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(UNICORN_CFLAGS) $(RAYLIB_CFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

# make run: the built-in defaults (proj7 via GIOS, quirks=default, halt on a quirk hit, IRQs, random DDR)
run: $(TARGET)
	$(VV_RUN) $(EXTRA)
# standard MIPS32: no quirks emulated (so none to halt on; guest-bug detectors still warn)
run-std: $(TARGET)
	$(VV_RUN) --quirks=none $(EXTRA)
# the hardware before fcdda7f (jalr always linked $31, lb/lh zero-extended)
run-legacy-hw: $(TARGET)
	$(VV_RUN) --quirks=+jalr-rd,+lb-sign $(EXTRA)
# keep running through quirk hits, logging each (quirk, pc) once
run-warn: $(TARGET)
	$(VV_RUN) --warn-on-quirk $(EXTRA)
# debugging: log every MMIO access / GP launch, and warn on all quirks (also non-emulated ones)
run-debug: $(TARGET)
	$(VV_RUN) --mmio-log --warn-on-quirk=all $(EXTRA)
# no interrupts taken
run-noirq: $(TARGET)
	$(VV_RUN) --no-irq $(EXTRA)
# GIOS only: no app loaded, no autorun (type at the GIOS prompt in the window or terminal)
run-gios: $(TARGET)
	$(VV_RUN) --bare $(EXTRA)

test-player: $(TARGET)
	python3 tests/test_player.py

test-quirks: $(TARGET) tests
	python3 tests/test_quirks.py

test-hex: $(TARGET) tests
	python3 tests/test_hex.py

test-hw: $(TARGET) tests
	python3 tests/test_hw.py

# All tests (hex programs + hwprobe, hardware model + guest-bug detectors, quirk detection/emulation,
# proj7 player via GIOS)
test: check-hex test-hex test-hw test-quirks test-player

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

fresh: clean
	$(MAKE) all
