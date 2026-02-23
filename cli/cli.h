/*
 * cli.h — Shared CLI helpers
 */

#ifndef ALLTRAX_CLI_H
#define ALLTRAX_CLI_H

#include "alltrax.h"
#include <stdbool.h>

/* Global flags parsed from argv */
typedef struct {
    bool no_cal;
    bool no_verify;
    bool force;
} cli_flags;

/* Parse global flags from argv. Returns index of first non-flag arg. */
int cli_parse_flags(int argc, char** argv, cli_flags* flags);

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

#endif /* ALLTRAX_CLI_H */
