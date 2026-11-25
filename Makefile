# Makefile — alltrax CLI + library
#
# Usage:
#   make              — release build (build/release/)
#   make DEBUG=1      — debug build with ASan (build/debug/)
#   make SANITIZE=1   — ASan + UBSan incl. float-cast-overflow (build/sanitize/)
#   make test         — build and run tests (combine with the variables above)
#   make CC=clang-22 VARIANT=clang test — build and test with clang in its own
#                       directory (use a distinct VARIANT per compiler)
#   make EMBEDDED_KEYS=0 — build without the built-in config and field-map keys
#                       (build/<variant>-nokeys/)
#   make clean        — remove build directory

CC       ?= gcc
AR       ?= ar
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS := -Isrc

# --- Debug / Release flags ---

ifdef SANITIZE
  SANITIZE_FLAGS := -fsanitize=address,undefined,float-cast-overflow \
                    -fno-sanitize-recover=all
  CFLAGS  += -O1 -g -fno-omit-frame-pointer $(SANITIZE_FLAGS)
  LDFLAGS += $(SANITIZE_FLAGS)
  VARIANT ?= sanitize
else ifdef DEBUG
  CFLAGS  += -O0 -ggdb -DDEBUG=1 -fsanitize=address
  LDFLAGS += -fsanitize=address
  VARIANT ?= debug
else
  CFLAGS  += -O2 -DNDEBUG
  VARIANT ?= release
endif

UNAME := $(shell uname)

# hidapi is required: every library operation goes through it.
ifeq ($(UNAME),Linux)
  HIDAPI_PKG := hidapi-hidraw
else
  HIDAPI_PKG := hidapi
endif
ifneq ($(MAKECMDGOALS),clean)
  ifneq ($(shell pkg-config --exists $(HIDAPI_PKG) 2>/dev/null && echo yes),yes)
    $(error pkg-config cannot find $(HIDAPI_PKG). Install pkg-config and libhidapi-dev (Debian/Ubuntu) or hidapi (Homebrew/MacPorts))
  endif
endif
HIDAPI_CFLAGS := $(shell pkg-config --cflags $(HIDAPI_PKG) 2>/dev/null)
HIDAPI_LIBS   := $(shell pkg-config --libs $(HIDAPI_PKG) 2>/dev/null)

# Optional libraries for config files. libxml2 enables `config save` and
# `config load`; libcrypto adds encrypted, Toolkit-compatible files. Without
# libcrypto, config files are plain XML only.
HAS_XML    := $(shell pkg-config --exists libxml-2.0 2>/dev/null && echo yes)
HAS_CRYPTO := $(shell pkg-config --exists libcrypto 2>/dev/null && echo yes)

ifeq ($(HAS_XML),yes)
  XML_CFLAGS := $(shell pkg-config --cflags libxml-2.0)
  XML_LIBS   := $(shell pkg-config --libs libxml-2.0)
  CPPFLAGS   += -DALLTRAX_HAVE_XML
  ifeq ($(HAS_CRYPTO),yes)
    CRYPTO_CFLAGS := $(shell pkg-config --cflags libcrypto)
    CRYPTO_LIBS   := $(shell pkg-config --libs libcrypto)
    CPPFLAGS      += -DALLTRAX_HAVE_CRYPTO
  else
    $(info Note: libcrypto not found (OpenSSL development files, e.g. libssl-dev) — config files are plain XML only)
  endif
else
  HAS_CRYPTO :=
  $(info Note: libxml2 not found (e.g. libxml2-dev) — building without config save/load)
endif

# Built-in keys, used only for encrypted config and field-map files
EMBEDDED_KEYS ?= 1
ifeq ($(EMBEDDED_KEYS),1)
  BUILD := build/$(VARIANT)
  ifeq ($(HAS_CRYPTO),yes)
    KEY_CPPFLAGS := -DALLTRAX_EMBEDDED_KEYS
    CPPFLAGS += $(KEY_CPPFLAGS)
  endif
else
  BUILD := build/$(VARIANT)-nokeys
endif

FLAGS_FILE := $(BUILD)/flags
FLAGS_TEXT := $(CC) $(CFLAGS) $(CPPFLAGS) $(HIDAPI_CFLAGS) $(CRYPTO_CFLAGS) $(XML_CFLAGS) $(LDFLAGS) $(XML_LIBS) $(CRYPTO_LIBS)

# --- Sources ---

LIB_SRC  := src/protocol.c src/transport.c src/controller.c src/variables.c
CLI_SRC  := cli/main.c cli/args.c cli/current.c cli/field_map.c cli/cmd_info.c cli/cmd_devices.c cli/cmd_get.c cli/cmd_write.c cli/cmd_reset.c cli/cmd_restore_defaults.c cli/cmd_fan_test.c cli/cmd_monitor.c cli/cmd_errors.c cli/cmd_curve.c cli/cmd_throttle.c cli/cmd_field_map.c cli/rules.c $(if $(HAS_XML),cli/cmd_config.c)
TEST_SRC := test/test_main.c test/test_protocol.c test/test_variables.c test/test_curves.c

LIB_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(LIB_SRC)))
CLI_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(CLI_SRC)))
TEST_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(TEST_SRC)))

LIB      := $(BUILD)/liballtrax-usb.a
CLI      := $(BUILD)/alltrax
TEST_BIN := $(BUILD)/test_alltrax
CONTROLLER_TEST_BIN := $(BUILD)/test_controller
CLI_WRITE_TEST_BIN := $(BUILD)/test_cli_write
TRANSPORT_TEST_BIN := $(BUILD)/test_transport
NOKEYS_TEST_BIN := $(if $(KEY_CPPFLAGS),$(BUILD)/test_cli_write_nokeys)

# --- Targets ---

COMPDB  := $(BUILD)/compile_commands.json

ALL_SRC := $(LIB_SRC) $(CLI_SRC) $(TEST_SRC) test/test_controller.c test/test_cli_write.c test/test_transport.c
all: $(CLI) $(TEST_BIN) $(CONTROLLER_TEST_BIN) $(CLI_WRITE_TEST_BIN) $(NOKEYS_TEST_BIN) $(TRANSPORT_TEST_BIN) $(COMPDB)

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(CLI): $(CLI_OBJ) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $(CLI_OBJ) $(LIB) $(HIDAPI_LIBS) $(CRYPTO_LIBS) $(XML_LIBS) -lm

$(TEST_BIN): $(TEST_OBJ) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJ) $(LIB) $(HIDAPI_LIBS) -lm

# Controller tests supply a simulated transport; never link the USB transport.
$(CONTROLLER_TEST_BIN): $(BUILD)/test_controller.o $(filter-out $(BUILD)/transport.o,$(LIB_OBJ))
	$(CC) $(LDFLAGS) -o $@ $^ -lm

# CLI preflight tests use real argument parsing and controller validation with
# a fake transport/open. Config coverage follows the optional libraries.
CLI_TEST_CONFIG_OBJ := $(if $(HAS_XML),$(BUILD)/cmd_config.o)
CLI_TEST_CONFIG_LIBS := $(CRYPTO_LIBS) $(XML_LIBS)
CLI_WRITE_TEST_OBJ := $(BUILD)/args.o $(BUILD)/current.o $(BUILD)/field_map.o $(BUILD)/rules.o $(BUILD)/cmd_get.o $(BUILD)/cmd_write.o $(BUILD)/cmd_curve.o $(BUILD)/cmd_throttle.o $(BUILD)/cmd_monitor.o $(BUILD)/cmd_reset.o $(BUILD)/cmd_restore_defaults.o $(BUILD)/cmd_fan_test.o $(filter-out $(BUILD)/transport.o,$(LIB_OBJ))
$(CLI_WRITE_TEST_BIN): $(BUILD)/test_cli_write.o $(CLI_TEST_CONFIG_OBJ) $(BUILD)/cmd_field_map.o $(CLI_WRITE_TEST_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(CLI_TEST_CONFIG_LIBS) -lm

# A keyed build also tests config and field-map handling as built without the keys.
ifneq ($(NOKEYS_TEST_BIN),)
$(NOKEYS_TEST_BIN): $(BUILD)/test_cli_write_nokeys.o $(BUILD)/cmd_config_nokeys.o $(BUILD)/cmd_field_map_nokeys.o $(CLI_WRITE_TEST_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(CLI_TEST_CONFIG_LIBS) -lm

$(BUILD)/%_nokeys.o: %.c $(FLAGS_FILE) | $(BUILD)
	$(CC) $(CFLAGS) $(filter-out $(KEY_CPPFLAGS),$(CPPFLAGS)) $(CRYPTO_CFLAGS) $(XML_CFLAGS) -MMD -MP -c -o $@ $<
endif

# Transport tests supply hidapi symbols; never link the real USB library.
$(TRANSPORT_TEST_BIN): $(BUILD)/test_transport.o $(BUILD)/cmd_devices.o $(BUILD)/args.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

test: $(TEST_BIN) $(CONTROLLER_TEST_BIN) $(CLI_WRITE_TEST_BIN) $(NOKEYS_TEST_BIN) $(TRANSPORT_TEST_BIN)
	./$(TEST_BIN)
	./$(CONTROLLER_TEST_BIN)
	./$(CLI_WRITE_TEST_BIN)
	$(if $(NOKEYS_TEST_BIN),./$(NOKEYS_TEST_BIN))
	./$(TRANSPORT_TEST_BIN)

clean:
	rm -rf build

# --- Compile rules ---

VPATH := src cli test

$(BUILD)/%.o: %.c $(FLAGS_FILE) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(HIDAPI_CFLAGS) $(CRYPTO_CFLAGS) $(XML_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $@

# Objects depend on this file, which changes only when the compiler, flags or
# detected libraries change, so installing a library rebuilds what uses it.
$(FLAGS_FILE): FORCE | $(BUILD)
	@printf '%s\n' '$(FLAGS_TEXT)' | cmp -s - $@ || printf '%s\n' '$(FLAGS_TEXT)' > $@

FORCE:

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

.PHONY: all test clean FORCE
