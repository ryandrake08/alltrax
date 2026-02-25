# Makefile — alltrax CLI + library
#
# Usage:
#   make              — release build (build/release/)
#   make DEBUG=1      — debug build with ASan (build/debug/)
#   make test         — build and run tests
#   make clean        — remove build directory

CC       ?= gcc
AR       ?= ar
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS := -Isrc

# --- Debug / Release flags ---

ifdef DEBUG
  CFLAGS  += -O0 -ggdb -DDEBUG=1 -fsanitize=address
  LDFLAGS += -fsanitize=address
  VARIANT := debug
else
  CFLAGS  += -O2 -DNDEBUG
  VARIANT := release
endif

UNAME := $(shell uname)

ifeq ($(UNAME),Linux)
  HIDAPI_CFLAGS := $(shell pkg-config --cflags hidapi-hidraw)
  HIDAPI_LIBS   := $(shell pkg-config --libs hidapi-hidraw)
else
  HIDAPI_CFLAGS := $(shell pkg-config --cflags hidapi)
  HIDAPI_LIBS   := $(shell pkg-config --libs hidapi)
endif

# CLI requires libcrypto + libxml2; library and tests do not
HAS_CRYPTO := $(shell pkg-config --exists libcrypto 2>/dev/null && echo yes)
HAS_XML    := $(shell pkg-config --exists libxml-2.0 2>/dev/null && echo yes)

ifeq ($(HAS_CRYPTO)$(HAS_XML),yesyes)
  CRYPTO_CFLAGS := $(shell pkg-config --cflags libcrypto)
  CRYPTO_LIBS   := $(shell pkg-config --libs libcrypto)
  XML_CFLAGS    := $(shell pkg-config --cflags libxml-2.0)
  XML_LIBS      := $(shell pkg-config --libs libxml-2.0)
  BUILD_CLI     := yes
else
  $(info Note: libcrypto or libxml2 not found — building library and tests only (CLI skipped))
  BUILD_CLI     :=
endif

BUILD := build/$(VARIANT)

# --- Sources ---

LIB_SRC  := src/protocol.c src/transport.c src/controller.c src/variables.c
CLI_SRC  := cli/main.c cli/cmd_info.c cli/cmd_get.c cli/cmd_write.c cli/cmd_reset.c cli/cmd_monitor.c cli/cmd_errors.c cli/cmd_config.c cli/cmd_curve.c
TEST_SRC := test/test_main.c test/test_protocol.c test/test_variables.c test/test_curves.c

LIB_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(LIB_SRC)))
CLI_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(CLI_SRC)))
TEST_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(TEST_SRC)))

LIB      := $(BUILD)/liballtrax-usb.a
CLI      := $(BUILD)/alltrax
TEST_BIN := $(BUILD)/test_alltrax

# --- Targets ---

COMPDB  := $(BUILD)/compile_commands.json

ifeq ($(BUILD_CLI),yes)
  ALL_SRC := $(LIB_SRC) $(CLI_SRC) $(TEST_SRC)
  all: $(CLI) $(TEST_BIN) $(COMPDB)
else
  ALL_SRC := $(LIB_SRC) $(TEST_SRC)
  all: $(LIB) $(TEST_BIN) $(COMPDB)
endif

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(CLI): $(CLI_OBJ) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $(CLI_OBJ) $(LIB) $(HIDAPI_LIBS) $(CRYPTO_LIBS) $(XML_LIBS) -lm

$(TEST_BIN): $(TEST_OBJ) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJ) $(LIB) $(HIDAPI_LIBS) -lm

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf build

# --- Compile rules ---

VPATH := src cli test

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(HIDAPI_CFLAGS) $(CRYPTO_CFLAGS) $(XML_CFLAGS) -MMD -MP -c -o $@ $<

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
		for flag in $(CFLAGS) $(CPPFLAGS) $(HIDAPI_CFLAGS) $(CRYPTO_CFLAGS) $(XML_CFLAGS); do \
			printf ', "%s"' "$$flag" >> $@; \
		done; \
		printf ', "-c", "%s"] }\n' "$$src" >> $@; \
		sep=,; \
	done
	@printf ']\n' >> $@

.PHONY: all test clean
