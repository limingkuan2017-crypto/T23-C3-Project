CONFIG_UCLIBC_BUILD ?= y

T23_FW_ROOT := $(abspath ../..)
REPO_ROOT := $(abspath $(T23_FW_ROOT)/..)
SDK_ROOT ?= $(REPO_ROOT)/third_party/ingenic_t23_sdk
SDK_INC_DIR := $(SDK_ROOT)/sdk/include
SDK_LIB_DIR := $(SDK_ROOT)/sdk/lib/uclibc
VENDOR_REF ?= $(REPO_ROOT)/third_party/vendor_reference
TOOLCHAIN_BIN ?= $(VENDOR_REF)/build/toolchain/mips-gcc540-glibc222-64bit-r3.3.0.smaller/bin

ifeq ($(wildcard $(TOOLCHAIN_BIN)/mips-linux-gnu-gcc),)
CROSS_COMPILE ?= mips-linux-gnu-
else
CROSS_COMPILE ?= $(TOOLCHAIN_BIN)/mips-linux-gnu-
endif

CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
STRIP = $(CROSS_COMPILE)strip

TARGET = t23_camera_diag
BUILD_DIR = build
SRC_DIR = src
OUT_DIR = $(T23_FW_ROOT)/output

SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/camera_common.c
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPFILES = $(OBJS:.o=.d)

CFLAGS = -O2 -Wall -Wextra -march=mips32r2
ifeq ($(CONFIG_UCLIBC_BUILD),y)
CFLAGS += -muclibc
endif
CFLAGS += -Iinclude -I$(SDK_INC_DIR)

LDFLAGS = -Wl,-gc-sections
ifeq ($(CONFIG_UCLIBC_BUILD),y)
LDFLAGS += -muclibc
endif
LIBS = $(SDK_LIB_DIR)/libimp.a $(SDK_LIB_DIR)/libalog.a -lpthread -lm -lrt -ldl

.PHONY: all clean

all: $(OUT_DIR)/$(TARGET)

$(OUT_DIR)/$(TARGET): $(OBJS) | $(OUT_DIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)
	$(STRIP) $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -MMD -MP -c $(CFLAGS) $< -o $@

$(BUILD_DIR):
	mkdir -p $@

$(OUT_DIR):
	mkdir -p $@

-include $(DEPFILES)

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)/$(TARGET)
