T23_FW_ROOT := $(abspath ../..)
REPO_ROOT := $(abspath $(T23_FW_ROOT)/..)
VENDOR_REF ?= $(REPO_ROOT)/third_party/vendor_reference
TOOLCHAIN_BIN ?= $(VENDOR_REF)/build/toolchain/mips-gcc540-glibc222-64bit-r3.3.0.smaller/bin

ifeq ($(wildcard $(TOOLCHAIN_BIN)/mips-linux-gnu-gcc),)
CROSS ?= mips-linux-gnu-
else
CROSS ?= $(TOOLCHAIN_BIN)/mips-linux-gnu-
endif

CC := $(CROSS)gcc
STRIP := $(CROSS)strip

CFLAGS += -O2 -Wall -Wextra -march=mips32r2 -muclibc
LDFLAGS += -muclibc

BUILD_DIR := build
TARGET := $(T23_FW_ROOT)/output/t23_spi_diag

SRCS := src/main.c
OBJS := $(SRCS:src/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) -Iinclude -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)
