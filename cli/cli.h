/*
 * cli.h — Shared CLI helpers
 */

#ifndef ALLTRAX_CLI_H
#define ALLTRAX_CLI_H

#include "alltrax.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* main.c: global options, opening the controller, errors             */
/* ------------------------------------------------------------------ */

/* Global --device path (NULL = auto-detect) */
extern const char* cli_device_path;

/* Open controller, printing errors to stderr. Returns ALLTRAX_OK on success. */
alltrax_error cli_open(alltrax_controller** ctrl, bool writes);

/* Print error with context to stderr */
void cli_error(alltrax_controller* ctrl, alltrax_error err, const char* action);

/* ------------------------------------------------------------------ */
/* args.c: option parsing and write options                           */
/* ------------------------------------------------------------------ */

/* Global --debug-ignore-fw-bounds: cli_open() enables the library firmware
 * read override. It never widens writes. */
extern bool cli_ignore_fw_bounds;

/* Options a command accepts, passed to cli_parse_args() */
typedef uint64_t cli_option_mask;

/* Bits 0–31 are reserved for user options. */
/* Writes: firmware policy and bulk-write confirmations */
#define CLI_OPT_ALLOW_UNVALIDATED   (UINT64_C(1) << 0)
#define CLI_OPT_THROTTLE            (UINT64_C(1) << 1)
#define CLI_OPT_FIELD_MAP           (UINT64_C(1) << 2)
/* File encryption keys */
#define CLI_OPT_KEY                 (UINT64_C(1) << 3)  /* takes a value */
/* get */
#define CLI_OPT_FACTORY             (UINT64_C(1) << 4)
/* curve */
#define CLI_OPT_FILE                (UINT64_C(1) << 5)  /* takes a value */
#define CLI_OPT_PRESET              (UINT64_C(1) << 6)  /* takes a value */
#define CLI_OPT_PLOT                (UINT64_C(1) << 7)
/* monitor */
#define CLI_OPT_MONITOR_RATE        (UINT64_C(1) << 8) /* takes a value */
#define CLI_OPT_MONITOR_LOG         (UINT64_C(1) << 9) /* takes a value */
#define CLI_OPT_WATCH               (UINT64_C(1) << 10) /* takes a value */
/* Skip the value rules for settings and curves */
#define CLI_OPT_SKIP_RULES          (UINT64_C(1) << 11)

/* Options accepted by every command that writes settings */
#define CLI_OPTS_WRITE CLI_OPT_ALLOW_UNVALIDATED

/* Options parsed from argv */
typedef struct {
    bool allow_unvalidated_firmware;
    bool throttle;
    bool field_map;
    bool plot;
    bool factory;
    bool skip_rules;
    const char* file;    /* --file value, or NULL */
    const char* preset;  /* --preset value, or NULL */
    const char* key;     /* --key value, or NULL */
    const char* monitor_rate;
    const char* monitor_log;
    const char* watch;   /* --watch value, or NULL */
} cli_flags;

/* Parse options anywhere in argv[1..argc-1]. Arguments starting with "--" are
 * options; only those in `allowed` are accepted. Positional arguments are moved
 * to argv[1..] in their original order and must number min_args..max_args
 * (max_args < 0: no limit). Returns the positional count, or -1 after printing
 * an error naming `command`. */
int cli_parse_args(int argc, char** argv, const char* command,
    cli_option_mask allowed, int min_args, int max_args, cli_flags* flags);

/* Print the options in `allowed` under an "Options:" heading. */
void cli_print_options(FILE* out, cli_option_mask allowed);

/* Sleep until interval_ms after start (CLOCK_MONOTONIC); a signal ends the
 * sleep early. */
void cli_wait_interval(const struct timespec* start, int interval_ms);

/* Library write options for the parsed flags. report may be NULL. */
alltrax_write_opts cli_write_opts(const cli_flags* flags,
    alltrax_write_report* report);

/* Print the actions a write performed besides writing its batch, and wait
 * for the controller to reconnect after a reset. */
void cli_report_write(alltrax_controller* ctrl, const alltrax_write_report* report);

/* Wait for the controller to reconnect after a reset, printing the outcome. */
void cli_wait_for_reconnect(alltrax_controller* ctrl);

/* ------------------------------------------------------------------ */
/* field_map.c: throttle and field-map write guard                    */
/* ------------------------------------------------------------------ */

/* Settings that must match installed hardware. Field-map data: the writable
 * Field variables and the field and genfield curves. Throttle settings: the
 * writable Throttle variables and the linearization, speed and torque curves. */
bool cli_is_field_map_var(const alltrax_var_def* var);
bool cli_is_field_map_curve(const alltrax_curve_def* def);
bool cli_assignment_is_field_map(const alltrax_assignment* item);
bool cli_assignment_is_throttle(const alltrax_assignment* item);

/* Bulk commands write throttle settings only with --throttle and field-map
 * data only with --field-map. `what` names the command or file in the error.
 * Return 0 to proceed, 1 after printing which options are missing. */
int cli_require_groups(const char* what, bool throttle, bool field_map,
    const cli_flags* flags);
int cli_check_bulk(const char* what, const alltrax_assignment* items, size_t count,
    const cli_flags* flags);

/* Print the motor-match warning when the batch writes field-map data. */
void cli_warn_field_map(const alltrax_assignment* items, size_t count);

/* When the batch writes field-map data without F_Table_Name, append the
 * name from the page the write will use (items needs room for one more).
 * `name` holds the text for the appended item. */
alltrax_error cli_add_field_map_name(alltrax_controller* ctrl,
    alltrax_assignment* items, size_t* count, char name[33]);

/* After a successful field-map write, remind the user about the pole count. */
void cli_field_map_written(const alltrax_assignment* items, size_t count);

/* ------------------------------------------------------------------ */
/* current.c: present settings and profile currents                   */
/* ------------------------------------------------------------------ */

/* Read user settings from the page a settings write starts from: the user
 * page, or the factory page while the user page is invalid (the write
 * restores it first). */
alltrax_error cli_read_current_settings(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out);

/* Profile current fields are stored as percentages of rated current. */
bool cli_is_profile_current(const alltrax_var_def* var);
int cli_convert_profile_currents(alltrax_controller* ctrl,
    alltrax_assignment* items, const bool* in_amps, size_t count,
    alltrax_info* info, bool* have_info);

/* ------------------------------------------------------------------ */
/* rules.c: value rules                                               */
/* ------------------------------------------------------------------ */

/* Value rules for settings and curves entered through the CLI (rules.c).
 * Both return 0 to proceed, or 1 after printing the broken rule; with
 * --skip-rules they print a warning and return 0.
 * cli_rules_check_curve() needs no device. cli_rules_check() checks every
 * setting and curve in a batch, reading controller information only when a
 * rule needs it; `info` may pass information the caller already has. */
int cli_rules_check_curve(const alltrax_curve_data* curve, const cli_flags* flags);
int cli_rules_check(alltrax_controller* ctrl, const alltrax_assignment* items,
    size_t count, const alltrax_info* info, const cli_flags* flags);

/* ------------------------------------------------------------------ */
/* cmd_config.c: config documents (libxml2 builds)                    */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* cmd_*.c: subcommand handlers                                       */
/* ------------------------------------------------------------------ */

/* Each returns 0 on success, 1 on error. */
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
int cmd_debug_read(int argc, char** argv);

#endif /* ALLTRAX_CLI_H */
