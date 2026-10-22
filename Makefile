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

BIN  ?=
ARGS ?=

.PHONY: all clean fresh run

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
	@if [ -z "$(BIN)" ]; then echo "usage: make run BIN=path/to/binary [ARGS=...]"; exit 1; fi
	LD_LIBRARY_PATH=$(HOME)/.local/lib ./$(TARGET) --bios $(BIOS) $(ARGS) $(BIN)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

fresh: clean
	$(MAKE) all
