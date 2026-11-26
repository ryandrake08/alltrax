/*
 * args.c — Shared command-line option parsing
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cli.h"

bool cli_ignore_fw_bounds = false;

static const struct {
    const char* name;
    cli_option_mask bit;
    const char* value;  /* value placeholder for options that take one */
    const char* help;
} options[] = {
    { "--allow-unvalidated-firmware", CLI_OPT_ALLOW_UNVALIDATED, NULL,
      "Write to firmware V0.001-V5.999 not yet validated for writes" },
    { "--skip-rules", CLI_OPT_SKIP_RULES, NULL,
      "Skip the value rules for settings and curves" },
    { "--throttle", CLI_OPT_THROTTLE, NULL,
      "Let a bulk write include throttle settings (must match the installed throttle)" },
    { "--field-map", CLI_OPT_FIELD_MAP, NULL,
      "Let a bulk write include field-map data (must match the installed motor)" },
    { "--key", CLI_OPT_KEY, "<key>",
      "File key: auto (default), none for an unencrypted file, or <48 hex>:<16 hex>" },
    { "--factory", CLI_OPT_FACTORY, NULL,
      "Read the factory settings page" },
    { "--file", CLI_OPT_FILE, "<f.csv>",
      "Read or write curve points as CSV" },
    { "--preset", CLI_OPT_PRESET, "<name>",
      "Use a built-in preset curve" },
    { "--plot", CLI_OPT_PLOT, NULL,
      "Show an ASCII plot instead of a table" },
    { "--rate", CLI_OPT_MONITOR_RATE, "<ms>",
      "Full monitor polling interval: 100, 1000, or 3000 (default 100)" },
    { "--log", CLI_OPT_MONITOR_LOG, "<file.csv>",
      "Append full monitor samples to a CSV file" },
    { "--watch", CLI_OPT_WATCH, "<ms>",
      "Read again every <ms> milliseconds until Ctrl+C" },
};
#define OPTION_COUNT (sizeof(options) / sizeof(options[0]))

static cli_option_mask option_bit(const char* arg)
{
    for (size_t i = 0; i < OPTION_COUNT; i++) {
        if (strcmp(arg, options[i].name) == 0)
            return options[i].bit;
    }
    return 0;
}

void cli_print_options(FILE* out, cli_option_mask allowed)
{
    if (allowed)
        fprintf(out, "\nOptions:\n");
    for (size_t i = 0; i < OPTION_COUNT; i++) {
        if (!(options[i].bit & allowed))
            continue;
        char label[48];
        snprintf(label, sizeof(label), "%s%s%s", options[i].name,
                 options[i].value ? " " : "",
                 options[i].value ? options[i].value : "");
        fprintf(out, "  %-30s %s\n", label, options[i].help);
    }
}

#define RECONNECT_TIMEOUT_MS 7500u

void cli_wait_for_reconnect(alltrax_controller* ctrl)
{
    fflush(stdout);
    if (alltrax_wait_for_reconnect(ctrl, RECONNECT_TIMEOUT_MS) == ALLTRAX_OK)
        printf("Controller reconnected\n");
    else
        fprintf(stderr, "Warning: controller did not reappear within %.1f seconds "
                "after reset. If using USB passthrough, reconnect the cable.\n",
                RECONNECT_TIMEOUT_MS / 1000.0);
}

void cli_report_write(alltrax_controller* ctrl, const alltrax_write_report* report)
{
    if (report->factory_restored)
        fprintf(stderr, "User settings were invalid; factory settings were "
                "restored before writing.\n");
    if (report->autocal_cleared)
        printf("Cleared throttle auto-calibration\n");
    if (report->date_stamped)
        printf("Stamped field-map date\n");
    if (report->reset_sent) {
        printf("Device reset sent (controller will reboot)\n");
        cli_wait_for_reconnect(ctrl);
    }
}

alltrax_write_opts cli_write_opts(const cli_flags* flags,
    alltrax_write_report* report)
{
    return (alltrax_write_opts){
        .skip_fw_check = flags->allow_unvalidated_firmware,
        .report = report,
    };
}

int cli_parse_args(int argc, char** argv, const char* command,
    cli_option_mask allowed, int min_args, int max_args, cli_flags* flags)
{
    memset(flags, 0, sizeof(*flags));

    int npos = 0;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (strncmp(arg, "--", 2) != 0) {
            argv[1 + npos++] = argv[i];
            continue;
        }

        cli_option_mask bit = option_bit(arg);
        if (!bit) {
            fprintf(stderr, "Error: %s: unknown option %s\n", command, arg);
            return -1;
        }
        if (!(bit & allowed)) {
            fprintf(stderr, "Error: %s does not accept %s\n", command, arg);
            return -1;
        }

        const char** value = bit == CLI_OPT_KEY ? &flags->key
                           : bit == CLI_OPT_FILE ? &flags->file
                           : bit == CLI_OPT_PRESET ? &flags->preset
                           : bit == CLI_OPT_MONITOR_RATE ? &flags->monitor_rate
                           : bit == CLI_OPT_MONITOR_LOG ? &flags->monitor_log
                           : bit == CLI_OPT_WATCH ? &flags->watch
                           : NULL;
        if (value) {
            if (i + 1 >= argc || strncmp(argv[i + 1], "--", 2) == 0) {
                fprintf(stderr, "Error: %s: %s requires a value\n",
                        command, arg);
                return -1;
            }
            if (*value) {
                fprintf(stderr, "Error: %s: %s given more than once\n",
                        command, arg);
                return -1;
            }
            *value = argv[++i];
            continue;
        }

        switch (bit) {
        case CLI_OPT_ALLOW_UNVALIDATED: flags->allow_unvalidated_firmware = true; break;
        case CLI_OPT_THROTTLE:          flags->throttle = true; break;
        case CLI_OPT_FIELD_MAP:         flags->field_map = true; break;
        case CLI_OPT_FACTORY:           flags->factory = true; break;
        case CLI_OPT_PLOT:              flags->plot = true; break;
        case CLI_OPT_SKIP_RULES:        flags->skip_rules = true; break;
        default: break;
        }
    }

    if (max_args >= 0 && npos > max_args) {
        fprintf(stderr, "Error: %s: unexpected argument %s\n",
                command, argv[1 + max_args]);
        return -1;
    }
    if (npos < min_args) {
        fprintf(stderr, "Error: %s: missing required argument\n", command);
        return -1;
    }
    return npos;
}

void cli_wait_interval(const struct timespec* start, int interval_ms)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
    int64_t elapsed = (int64_t)(now.tv_sec - start->tv_sec) * 1000000000 +
                      now.tv_nsec - start->tv_nsec;
    int64_t remaining = (int64_t)interval_ms * 1000000 - elapsed;
    if (remaining > 0)
        nanosleep(&(struct timespec){ (time_t)(remaining / 1000000000),
                                      (long)(remaining % 1000000000) }, NULL);
}
