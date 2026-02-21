HOST_CC ?= gcc
T113_CC ?= arm-linux-gnueabihf-gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
LDFLAGS ?=
T113_LDFLAGS ?= -static

BUILD_DIR ?= build
HOST_DIR := $(BUILD_DIR)/host
T113_DIR := $(BUILD_DIR)/t113-static

SRC := uart_bl_update.c
HOST_BIN := $(HOST_DIR)/uart_bl_update
T113_BIN := $(T113_DIR)/uart_bl_update

.PHONY: all host t113 clean

all: host

host: $(HOST_BIN)

t113: $(T113_BIN)

$(HOST_BIN): $(SRC)
	mkdir -p $(HOST_DIR)
	$(HOST_CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(T113_BIN): $(SRC)
	mkdir -p $(T113_DIR)
	$(T113_CC) $(CFLAGS) $(LDFLAGS) $(T113_LDFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
