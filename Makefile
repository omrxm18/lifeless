# lifeless - Makefile
# Copyright (c) 2026 omrxm18 <omrxm18@proton.me>
# SPDX-License-Identifier: GPL-3.0-or-later

BINARY_NAME = lifeless
LSP_BINARY_NAME = lifeful
LSP_DIR 		= src/lsp
SRC_DIR     = src
OUT_DIR     = out
OBJ_DIR     = $(OUT_DIR)/obj

CC    ?= cc
STRIP ?= strip

COMMON_SRCS := \
	$(SRC_DIR)/lib/arena.c \
	$(SRC_DIR)/lib/lexer.c \
	$(SRC_DIR)/lib/brain.c \
	$(SRC_DIR)/lib/parser.c \
	$(SRC_DIR)/lib/complaints.c

COMPILER_SRCS := \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/lib/life_support.c \
	$(COMMON_SRCS)
COMPILER_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(COMPILER_SRCS))

LSP_SRCS := \
	$(shell find $(LSP_DIR) -type f -name '*.c' | sort) \
	$(COMMON_SRCS)
LSP_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(LSP_SRCS))

DEPS := $(sort $(COMPILER_OBJS:.o=.d) $(LSP_OBJS:.o=.d))

NPROC ?= $(shell nproc 2>/dev/null || echo 1)

# The include path and the dependency flags have to survive a user supplied
# CFLAGS/CPPFLAGS, hence the "override" on those two assignments.
override CPPFLAGS += -I $(SRC_DIR)/include
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
override CFLAGS += -MMD -MP

LDFLAGS ?=
LDLIBS  ?=

find-cc = $(shell command -v $(1)-gcc 2>/dev/null)

binaries: $(OUT_DIR)/$(BINARY_NAME) $(OUT_DIR)/$(LSP_BINARY_NAME)
.DEFAULT_GOAL := binaries

.PHONY: all binaries lifeful help format x86_64 aarch64 riscv64

all: x86_64 aarch64 riscv64
	@echo "[+] all three targets built"

help:
	@echo "lifeless, a lifeless language"
	@echo ""
	@echo "targets:"
	@echo "  all               build for all targets"
	@echo "  clean             remove the out dir"
	@echo "  format            format source files with clang-format"
	@echo "  x86_64            cross build for x86_64-linux-musl"
	@echo "  aarch64           cross build for aarch64-linux-musl"
	@echo "  risv64            cross build for riscv64-linux-musl"
	@echo ""
	@echo "variables: CC, CFLAGS, CPPFLAGS, LDFLAGS, V=0 (quiet)"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/lsp/%.o: $(LSP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/$(BINARY_NAME): $(COMPILER_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(OUT_DIR)/$(LSP_BINARY_NAME): $(LSP_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

# Cross build for x86_64 Linux with musl. It uses its own output directory so
# the musl objects never mix with the host ones.
x86_64:
	@CROSS_CC="$(call find-cc,x86_64-linux-musl)"; \
	if [ -n "$$CROSS_CC" ]; then \
		echo "[+] Cross building with $$CROSS_CC"; \
		$(MAKE) -j$(NPROC) OUT_DIR=$(OUT_DIR)/x86_64 CC="$$CROSS_CC" \
			CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wpedantic -static" \
			LDFLAGS="-static"; \
	else \
		echo "Error: x86_64-linux-musl-gcc was not found"; exit 1; \
	fi

aarch64:
	@CROSS_CC="$(call find-cc,aarch64-linux-musl)"; \
	if [ -n "$$CROSS_CC" ]; then \
		echo "[+] Cross building with $$CROSS_CC"; \
		$(MAKE) -j$(NPROC) OUT_DIR=$(OUT_DIR)/aarch64 CC="$$CROSS_CC" \
			CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wpedantic -static" \
			LDFLAGS="-static -fno-link-libatomic"; \
	else \
		echo "Error: aarch64-linux-musl not found"; exit 1; \
	fi

riscv64:
	@CROSS_CC="$(call find-cc,riscv64-linux-musl)"; \
	if [ -n "$$CROSS_CC" ]; then \
		echo "[+] Cross building with $$CROSS_CC"; \
		$(MAKE) -j$(NPROC) OUT_DIR=$(OUT_DIR)/riscv64 CC="$$CROSS_CC" \
			CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wpedantic -static" \
			LDFLAGS="-static -fno-link-libatomic"; \
	else \
		echo "Error: riscv64-linux-musl not found"; exit 1; \
	fi

format:
	@command -v clang-format >/dev/null 2>&1 || { echo "Error: clang-format not found"; exit 1; }
	@find $(SRC_DIR) $(LSP_DIR) -name '*.c' -o -name '*.h' | xargs clang-format -i
	@echo "[+] Done"

-include $(DEPS)
