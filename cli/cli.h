/*
 * cli.h — Shared CLI helpers
 */

#ifndef ALLTRAX_CLI_H
#define ALLTRAX_CLI_H

#include "alltrax.h"
#include <stdbool.h>
#include <stdio.h>

/* Global --device path (NULL = auto-detect) */
extern const char* cli_device_path;

/* Global --debug-ignore-fw-bounds: cli_open() enables the library firmware
 * read override. It never widens writes. */
extern bool cli_ignore_fw_bounds;

/* Options a command accepts, passed to cli_parse_args() */
enum {
    /* User options use bits 0–15. */
    CLI_OPT_NO_VOLTAGE_LINK     = 1u << 0,
    CLI_OPT_ALLOW_UNVALIDATED   = 1u << 1,
    CLI_OPT_FIELD_MAP           = 1u << 2,
    CLI_OPT_CONFIG_KEY          = 1u << 3,  /* takes a value */
    CLI_OPT_RESET               = 1u << 4,
    CLI_OPT_PLOT                = 1u << 5,
    CLI_OPT_FILE                = 1u << 6,  /* takes a value */
    CLI_OPT_PRESET              = 1u << 7,  /* takes a value */
    CLI_OPT_NO_WAIT             = 1u << 8,
    CLI_OPT_FACTORY             = 1u << 9,
    CLI_OPT_KEEP_THROTTLE       = 1u << 10,
    CLI_OPT_KEEP_FIELD_MAP      = 1u << 11,
    CLI_OPT_DEFAULT_CURVE       = 1u << 12,
    CLI_OPT_NO_HPD              = 1u << 13,

    /* Debug and development bypasses */
    CLI_OPT_DEBUG_NO_CAL        = 1u << 16,
    CLI_OPT_DEBUG_NO_VERIFY     = 1u << 17,
    CLI_OPT_DEBUG_NO_GOODSET    = 1u << 18,
    CLI_OPT_DEBUG_NO_POST_WRITE = 1u << 19,
};

/* Options accepted by every command that writes settings */
#define CLI_OPTS_WRITE (CLI_OPT_ALLOW_UNVALIDATED | CLI_OPT_RESET | CLI_OPT_NO_WAIT | \
    CLI_OPT_DEBUG_NO_CAL | CLI_OPT_DEBUG_NO_VERIFY | CLI_OPT_DEBUG_NO_GOODSET | \
    CLI_OPT_DEBUG_NO_POST_WRITE)

/* Options parsed from argv */
typedef struct {
    bool no_voltage_link;
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
    bool debug_no_cal;
    bool debug_no_verify;
    bool debug_no_goodset;
    bool debug_no_post_write;
    const char* file;    /* --file value, or NULL */
    const char* preset;  /* --preset value, or NULL */
    const char* config_key;  /* --config-key value, or NULL */
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
 * Returns 0, or 1 after printing an error naming `command`. */
int cli_resolve_key(const char* command, const char* option, const char* value,
    const char* env, const cli_key* embedded, cli_key* out);

/* Parse options anywhere in argv[1..argc-1]. Arguments starting with "--" are
 * options; only those in `allowed` are accepted. Positional arguments are moved
 * to argv[1..] in their original order and must number min_args..max_args
 * (max_args < 0: no limit). Returns the positional count, or -1 after printing
 * an error naming `command`. */
int cli_parse_args(int argc, char** argv, const char* command,
    unsigned allowed, int min_args, int max_args, cli_flags* flags);

/* Print the options in `allowed`: user options, then debug bypasses under
 * their own heading. */
void cli_print_options(FILE* out, unsigned allowed);

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
int cmd_errors(int argc, char** argv);
int cmd_config(int argc, char** argv);
int cmd_curve(int argc, char** argv);
int cmd_throttle(int argc, char** argv);

#endif /* ALLTRAX_CLI_H */
