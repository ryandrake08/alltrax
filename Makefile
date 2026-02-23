# Makefile — alltrax CLI + library

CC       ?= gcc
AR       ?= ar
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS := -Isrc

UNAME := $(shell uname)
ifeq ($(UNAME),Linux)
  HIDAPI_CFLAGS := $(shell pkg-config --cflags hidapi-hidraw)
  HIDAPI_LIBS   := $(shell pkg-config --libs hidapi-hidraw)
else
  HIDAPI_CFLAGS := $(shell pkg-config --cflags hidapi)
  HIDAPI_LIBS   := $(shell pkg-config --libs hidapi)
endif

BUILD := build

# --- Sources ---

LIB_SRC  := src/protocol.c src/transport.c src/controller.c src/variables.c
CLI_SRC  := cli/main.c cli/cmd_info.c cli/cmd_get.c cli/cmd_write.c cli/cmd_reset.c cli/cmd_monitor.c cli/cmd_errors.c
TEST_SRC := test/test_main.c test/test_protocol.c test/test_variables.c

LIB_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(LIB_SRC)))
CLI_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(CLI_SRC)))
TEST_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(TEST_SRC)))

LIB      := $(BUILD)/liballtrax-usb.a
CLI      := $(BUILD)/alltrax
TEST_BIN := $(BUILD)/test_alltrax

# --- Targets ---

ALL_SRC := $(LIB_SRC) $(CLI_SRC) $(TEST_SRC)
COMPDB  := $(BUILD)/compile_commands.json

all: $(CLI) $(TEST_BIN) $(COMPDB)

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(CLI): $(CLI_OBJ) $(LIB)
	$(CC) -o $@ $(CLI_OBJ) $(LIB) $(HIDAPI_LIBS) -lm

$(TEST_BIN): $(TEST_OBJ) $(LIB)
	$(CC) -o $@ $(TEST_OBJ) $(LIB) $(HIDAPI_LIBS) -lm

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(BUILD)

# --- Compile rules ---

VPATH := src cli test

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(HIDAPI_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $@

-include $(wildcard $(BUILD)/*.d)

# --- compile_commands.json for clangd ---

$(COMPDB): Makefile | $(BUILD)
	@printf '[\n' > $@
	@sep=; \
	for src in $(ALL_SRC); do \
		printf '%s  { "directory": "%s", "file": "%s", "arguments": ["%s"' \
			"$$sep" "$(CURDIR)" "$$src" "$(CC)" >> $@; \
		for flag in $(CFLAGS) $(CPPFLAGS) $(HIDAPI_CFLAGS); do \
			printf ', "%s"' "$$flag" >> $@; \
		done; \
		printf ', "-c", "%s"] }\n' "$$src" >> $@; \
		sep=,; \
	done
	@printf ']\n' >> $@

.PHONY: all test clean
