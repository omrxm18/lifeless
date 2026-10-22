# lifeless - Makefile
# Copyright (c) 2026 omrxm18 <omrxm18@proton.me>
# SPDX-License-Identifier: GPL-3.0-or-later

BINARY_NAME = lifeless
SRC_DIR     = src
OUT_DIR     = out
OBJ_DIR     = $(OUT_DIR)/obj

CC    ?= cc
STRIP ?= strip

SRCS := $(shell find $(SRC_DIR) -type f -name '*.c' | sort)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

NPROC ?= $(shell nproc 2>/dev/null || echo 1)

# The include path and the dependency flags have to survive a user supplied
# CFLAGS/CPPFLAGS, hence the "override" on those two assignments.
override CPPFLAGS += -I $(SRC_DIR)/include
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
override CFLAGS += -MMD -MP

LDFLAGS ?=
LDLIBS  ?=

# Look up a cross compiler: "x86_64-linux-musl" -> "x86_64-linux-musl-gcc".
find-cc = $(shell command -v $(1)-gcc 2>/dev/null)

.DEFAULT_GOAL := $(OUT_DIR)/$(BINARY_NAME)

.PHONY: all help clean x86_64

all: $(OUT_DIR)/$(BINARY_NAME)

help:
	@echo "lifeless, a lifeless language"
	@echo ""
	@echo "targets:"
	@echo "  all               build $(OUT_DIR)/$(BINARY_NAME) (default)"
	@echo "  clean             remove the out dir"
	@echo "  format            format source files with clang-format"
	@echo "  x86_64            cross build for x86_64-linux-musl"
	@echo ""
	@echo "variables: CC, CFLAGS, CPPFLAGS, LDFLAGS, V=0 (quiet)"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@


$(OUT_DIR)/$(BINARY_NAME): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

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

format:
	@command -v clang-format >/dev/null 2>&1 || { echo "Error: clang-format not found"; exit 1; }
	@find $(SRC_DIR) -name '*.c' -o -name '*.h' | xargs clang-format -i
	@echo "[+] Done"

clean:
	@rm -rf $(OUT_DIR)

-include $(DEPS)
