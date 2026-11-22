/*
 * cli.h — Shared CLI helpers
 */

#ifndef ALLTRAX_CLI_H
#define ALLTRAX_CLI_H

#include "alltrax.h"
#include <stdbool.h>

/* Global --device path (NULL = auto-detect) */
extern const char* cli_device_path;

/* Global --no-fw-version: enables the library firmware read override and
 * sets no_fw_version in every cli_parse_args() result. */
extern bool cli_no_fw_version;

/* Options a command accepts, passed to cli_parse_args() */
enum {
    CLI_OPT_NO_CAL          = 1u << 0,
    CLI_OPT_NO_VERIFY       = 1u << 1,
    CLI_OPT_NO_GOODSET      = 1u << 2,
    CLI_OPT_NO_FW_VERSION   = 1u << 3,
    CLI_OPT_NO_CRYPT        = 1u << 4,
    CLI_OPT_NO_VOLTAGE_LINK = 1u << 5,
    CLI_OPT_RESET           = 1u << 6,
    CLI_OPT_PLOT            = 1u << 7,
    CLI_OPT_FILE            = 1u << 8,  /* takes a value */
    CLI_OPT_PRESET          = 1u << 9,  /* takes a value */
};

/* Options parsed from argv */
typedef struct {
    bool no_cal;
    bool no_verify;
    bool no_goodset;
    bool no_fw_version;
    bool no_crypt;
    bool no_voltage_link;
    bool reset;
    bool plot;
    const char* file;    /* --file value, or NULL */
    const char* preset;  /* --preset value, or NULL */
} cli_flags;

/* Parse options anywhere in argv[1..argc-1]. Arguments starting with "--" are
 * options; only those in `allowed` are accepted. Positional arguments are moved
 * to argv[1..] in their original order and must number min_args..max_args
 * (max_args < 0: no limit). Returns the positional count, or -1 after printing
 * an error naming `command`. */
int cli_parse_args(int argc, char** argv, const char* command,
    unsigned allowed, int min_args, int max_args, cli_flags* flags);

/* Open controller, printing errors to stderr. Returns ALLTRAX_OK on success. */
alltrax_error cli_open(alltrax_controller** ctrl, bool writes);

/* Print error with context to stderr */
void cli_error(alltrax_controller* ctrl, alltrax_error err, const char* action);

/* Subcommand handlers — each returns 0 on success, 1 on error */
int cmd_info(int argc, char** argv);
int cmd_get(int argc, char** argv);
int cmd_write(int argc, char** argv);
int cmd_reset(int argc, char** argv);
int cmd_monitor(int argc, char** argv);
int cmd_errors(int argc, char** argv);
int cmd_config(int argc, char** argv);
int cmd_curve(int argc, char** argv);

#endif /* ALLTRAX_CLI_H */
