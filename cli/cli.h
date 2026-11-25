/*
 * cli.h — Shared CLI helpers
 */

#ifndef ALLTRAX_CLI_H
#define ALLTRAX_CLI_H

#include "alltrax.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Global --device path (NULL = auto-detect) */
extern const char* cli_device_path;

/* Global --debug-ignore-fw-bounds: cli_open() enables the library firmware
 * read override. It never widens writes. */
extern bool cli_ignore_fw_bounds;

/* Options a command accepts, passed to cli_parse_args() */
typedef uint64_t cli_option_mask;

/* Bits 0–31 are reserved for user options. */
/* Writes: firmware policy and field-map confirmation */
#define CLI_OPT_ALLOW_UNVALIDATED   (UINT64_C(1) << 0)
#define CLI_OPT_FIELD_MAP           (UINT64_C(1) << 1)
/* Reboot after a write or reset command */
#define CLI_OPT_RESET               (UINT64_C(1) << 2)
#define CLI_OPT_NO_WAIT             (UINT64_C(1) << 3)
/* File encryption keys */
#define CLI_OPT_CONFIG_KEY          (UINT64_C(1) << 4)  /* takes a value */
#define CLI_OPT_FIELD_MAP_KEY       (UINT64_C(1) << 5)  /* takes a value */
/* get */
#define CLI_OPT_FACTORY             (UINT64_C(1) << 6)
/* curve */
#define CLI_OPT_FILE                (UINT64_C(1) << 7)  /* takes a value */
#define CLI_OPT_PRESET              (UINT64_C(1) << 8)  /* takes a value */
#define CLI_OPT_PLOT                (UINT64_C(1) << 9)
/* restore-defaults */
#define CLI_OPT_KEEP_THROTTLE       (UINT64_C(1) << 10)
#define CLI_OPT_KEEP_FIELD_MAP      (UINT64_C(1) << 11)
/* throttle set */
#define CLI_OPT_DEFAULT_CURVE       (UINT64_C(1) << 12)
#define CLI_OPT_NO_HPD              (UINT64_C(1) << 13)
/* monitor */
#define CLI_OPT_MONITOR_RATE        (UINT64_C(1) << 14) /* takes a value */
#define CLI_OPT_MONITOR_LOG         (UINT64_C(1) << 15) /* takes a value */
#define CLI_OPT_MONITOR_THROTTLE    (UINT64_C(1) << 16)

/* Bits 32–63 are reserved for debug and development bypasses, in write
 * sequence order. */
#define CLI_OPT_DEBUG_NO_RULES      (UINT64_C(1) << 32)
#define CLI_OPT_DEBUG_NO_GOODSET    (UINT64_C(1) << 33)
#define CLI_OPT_DEBUG_NO_CAL        (UINT64_C(1) << 34)
#define CLI_OPT_DEBUG_NO_VERIFY     (UINT64_C(1) << 35)
#define CLI_OPT_DEBUG_NO_POST_WRITE (UINT64_C(1) << 36)

/* Options accepted by every command that writes settings */
#define CLI_OPTS_WRITE (CLI_OPT_ALLOW_UNVALIDATED | CLI_OPT_RESET | CLI_OPT_NO_WAIT | \
    CLI_OPT_DEBUG_NO_GOODSET | CLI_OPT_DEBUG_NO_CAL | CLI_OPT_DEBUG_NO_VERIFY | \
    CLI_OPT_DEBUG_NO_POST_WRITE)

/* Options parsed from argv */
typedef struct {
    bool allow_unvalidated_firmware;
    bool field_map;
    bool reset;
    bool plot;
    bool no_wait;
    bool factory;
    bool keep_throttle;
    bool keep_field_map;
    bool default_curve;
    bool no_hpd;
    bool monitor_throttle;
    bool debug_no_rules;
    bool debug_no_cal;
    bool debug_no_verify;
    bool debug_no_goodset;
    bool debug_no_post_write;
    const char* file;    /* --file value, or NULL */
    const char* preset;  /* --preset value, or NULL */
    const char* config_key;  /* --config-key value, or NULL */
    const char* fieldmap_key;  /* --fieldmap-key value, or NULL */
    const char* monitor_rate;
    const char* monitor_log;
} cli_flags;

/* A 3DES-CBC key and IV. encrypt is false when files are read and
 * written unencrypted. */
typedef struct {
    bool encrypt;
    unsigned char key[24];
    unsigned char iv[8];
} cli_key;

/* Resolve the key for `option` from its value (NULL means "auto"):
 *   none         unencrypted files
 *   <key>:<iv>   48 and 16 hex digits, used as given
 *   auto         the key in environment variable `env`, else `embedded`
 *                (NULL when no key is built in), else an error
 * The environment variable must hold <key>:<iv>; an empty one is ignored.
 * Builds without libcrypto accept only none.
 * Returns 0, or 1 after printing an error naming `command`. */
int cli_resolve_key(const char* command, const char* option, const char* value,
    const char* env, const cli_key* embedded, cli_key* out);

/* Print whether encrypted `what` files are available and whether a key is
 * built in (`embedded` is NULL when none is). */
void cli_print_key_status(FILE* out, const char* what, const char* option,
    const char* env, const cli_key* embedded);

/* Settings parsed from a config document (cmd_config.c, libxml2 builds). */
typedef struct {
    const alltrax_var_def* var;
    double number;   /* numeric and bool variables */
    char text[33];   /* string variables (at most 32 characters) */
} cli_config_entry;

#define CLI_CONFIG_MAX_CURVES 5

typedef struct {
    cli_config_entry* entries;
    size_t count;
    size_t skipped;  /* unknown elements */
    alltrax_curve_data curves[CLI_CONFIG_MAX_CURVES];  /* complete, validated */
    size_t curve_count;
} cli_config;

/* Read a file of at most 1 MiB, decrypting it unless key->encrypt is false.
 * Returns a NUL-terminated buffer, or NULL after printing an error; `option`
 * names the key option in hints. */
char* cli_read_file(const char* path, const cli_key* key, const char* option,
    size_t* len);

/* Parse a config document: writable variables and curves. Any invalid value
 * or curve fails the whole document. Returns 0, or 1 after printing an error. */
int cli_config_parse(const char* xml, size_t len, cli_config* out);
void cli_config_free(cli_config* config);

/* Assignments for every entry and curve, with room for one more item (the
 * field-map name). Items point into `config`. NULL when out of memory. */
alltrax_assignment* cli_config_assignments(const cli_config* config,
    size_t* count);

/* Write items in one batch, adding the field-map name when needed, then print
 * what was written and report post-write actions. Returns 0 or 1. */
int cli_config_write(alltrax_controller* ctrl, alltrax_assignment* items,
    size_t count, const cli_flags* flags);

/* Parse options anywhere in argv[1..argc-1]. Arguments starting with "--" are
 * options; only those in `allowed` are accepted. Positional arguments are moved
 * to argv[1..] in their original order and must number min_args..max_args
 * (max_args < 0: no limit). Returns the positional count, or -1 after printing
 * an error naming `command`. */
int cli_parse_args(int argc, char** argv, const char* command,
    cli_option_mask allowed, int min_args, int max_args, cli_flags* flags);

/* Print the options in `allowed`: user options, then debug bypasses under
 * their own heading. */
void cli_print_options(FILE* out, cli_option_mask allowed);

/* Library write options for the parsed flags. report may be NULL. */
alltrax_write_opts cli_write_opts(const cli_flags* flags,
    alltrax_write_report* report);

/* Print the actions a write performed besides writing its batch, and wait
 * for the controller to reconnect after a reset unless --no-wait. */
void cli_report_write(alltrax_controller* ctrl, const alltrax_write_report* report,
    const cli_flags* flags);

/* Wait for the controller to reconnect after a reset, printing the outcome. */
void cli_wait_for_reconnect(alltrax_controller* ctrl);

/* Open controller, printing errors to stderr. Returns ALLTRAX_OK on success. */
alltrax_error cli_open(alltrax_controller** ctrl, bool writes);

/* Print error with context to stderr */
void cli_error(alltrax_controller* ctrl, alltrax_error err, const char* action);

/* Field-map data: the writable Field variables and the field and genfield
 * curves. Writing it requires --field-map. */
bool cli_is_field_map_var(const alltrax_var_def* var);
bool cli_is_field_map_curve(const alltrax_curve_def* def);
bool cli_assignment_is_field_map(const alltrax_assignment* item);

/* Refuse field-map assignments without --field-map; with it, print the
 * motor-match warning. Returns 0 to proceed, 1 after printing an error. */
int cli_check_field_map(const alltrax_assignment* items, size_t count,
    bool allowed);

/* When the batch writes field-map data without F_Table_Name, append the
 * name from the page the write will use (items needs room for one more).
 * `name` holds the text for the appended item. */
alltrax_error cli_add_field_map_name(alltrax_controller* ctrl,
    alltrax_assignment* items, size_t* count, char name[33], bool skip_goodset);

/* After a successful field-map write, remind the user about the pole count. */
void cli_field_map_written(const alltrax_assignment* items, size_t count);

/* Profile current fields are stored as percentages of rated current. */
bool cli_is_profile_current(const alltrax_var_def* var);
int cli_convert_profile_currents(alltrax_controller* ctrl,
    alltrax_assignment* items, const bool* in_amps, size_t count,
    alltrax_info* info, bool* have_info);

/* Value rules for settings and curves entered through the CLI (rules.c).
 * Both return 0 to proceed, or 1 after printing the broken rule; with
 * --debug-no-rules they print a warning and return 0.
 * cli_rules_check_curve() needs no device. cli_rules_check() checks every
 * setting and curve in a batch, reading controller information only when a
 * rule needs it; `info` may pass information the caller already has. */
int cli_rules_check_curve(const alltrax_curve_data* curve, const cli_flags* flags);
int cli_rules_check(alltrax_controller* ctrl, const alltrax_assignment* items,
    size_t count, const alltrax_info* info, const cli_flags* flags);
/* restore-defaults --keep-throttle: the installed throttle type must be one
 * the hardware allows. */
int cli_rules_check_kept_throttle(alltrax_controller* ctrl, const cli_flags* flags);

/* Subcommand handlers — each returns 0 on success, 1 on error */
int cmd_info(int argc, char** argv);
int cmd_get(int argc, char** argv);
int cmd_write(int argc, char** argv);
int cmd_reset(int argc, char** argv);
int cmd_clear_autocal(int argc, char** argv);
int cmd_restore_defaults(int argc, char** argv);
int cmd_fan_test(int argc, char** argv);
int cmd_devices(int argc, char** argv);
int cmd_monitor(int argc, char** argv);
int cmd_field_map(int argc, char** argv);
int cmd_errors(int argc, char** argv);
int cmd_config(int argc, char** argv);
int cmd_curve(int argc, char** argv);
int cmd_throttle(int argc, char** argv);

#endif /* ALLTRAX_CLI_H */
