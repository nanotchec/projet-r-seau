CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude
LDFLAGS :=

BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj

COMMON_SRCS := src/common/protocol_io.c
COMMON_OBJS := $(COMMON_SRCS:src/%.c=$(OBJ_DIR)/%.o)

COMM_SRCS := src/comm/main.c
COMM_OBJS := $(COMM_SRCS:src/%.c=$(OBJ_DIR)/%.o)

DRIVER_SRCS := src/driver/main.c
DRIVER_OBJS := $(DRIVER_SRCS:src/%.c=$(OBJ_DIR)/%.o)

MOCK_SRCS := src/tools/mock_driver.c
MOCK_OBJS := $(MOCK_SRCS:src/%.c=$(OBJ_DIR)/%.o)

ALL_BINS := $(BIN_DIR)/comm $(BIN_DIR)/driver $(BIN_DIR)/mock_driver

.PHONY: all clean

all: $(ALL_BINS)

$(BIN_DIR)/comm: $(COMMON_OBJS) $(COMM_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/driver: $(COMMON_OBJS) $(DRIVER_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/mock_driver: $(COMMON_OBJS) $(MOCK_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
