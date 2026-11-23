/*
 * args.c — Shared command-line option parsing
 */

#include <stdio.h>
#include <string.h>
#include "cli.h"

bool cli_ignore_fw_bounds = false;

/* Debug bypasses are listed last and printed under their own heading. */
static const struct {
    const char* name;
    unsigned    bit;
    const char* value;  /* value placeholder for options that take one */
    const char* help;
} options[] = {
    { "--no-voltage-link", CLI_OPT_NO_VOLTAGE_LINK, NULL,
      "Skip the KSI/under-voltage/over-voltage relationship check" },
    { "--allow-unvalidated-firmware", CLI_OPT_ALLOW_UNVALIDATED, NULL,
      "Write to firmware V0.001-V5.999 not yet validated for writes" },
    { "--field-map", CLI_OPT_FIELD_MAP, NULL,
      "Allow writing field-map data (must match the installed motor)" },
    { "--no-crypt", CLI_OPT_NO_CRYPT, NULL,
      "Plain XML file instead of encrypted (not Toolkit-compatible)" },
    { "--reset", CLI_OPT_RESET, NULL,
      "Reboot the controller after writing" },
    { "--plot", CLI_OPT_PLOT, NULL,
      "Show an ASCII plot instead of a table" },
    { "--file", CLI_OPT_FILE, "<f.csv>",
      "Read or write curve points as CSV" },
    { "--preset", CLI_OPT_PRESET, "<name>",
      "Use a built-in preset curve" },
    { "--debug-no-cal", CLI_OPT_DEBUG_NO_CAL, NULL,
      "Skip the CAL/RUN mode bracket" },
    { "--debug-no-verify", CLI_OPT_DEBUG_NO_VERIFY, NULL,
      "Skip FLASH read-back verification" },
    { "--debug-no-goodset", CLI_OPT_DEBUG_NO_GOODSET, NULL,
      "Skip the GoodSet check (writes over invalid settings)" },
};
#define OPTION_COUNT (sizeof(options) / sizeof(options[0]))
#define DEBUG_OPTIONS (CLI_OPT_DEBUG_NO_CAL | CLI_OPT_DEBUG_NO_VERIFY | \
                       CLI_OPT_DEBUG_NO_GOODSET)

static unsigned option_bit(const char* arg)
{
    for (size_t i = 0; i < OPTION_COUNT; i++) {
        if (strcmp(arg, options[i].name) == 0)
            return options[i].bit;
    }
    return 0;
}

static void print_option_group(FILE* out, unsigned allowed)
{
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

void cli_print_options(FILE* out, unsigned allowed)
{
    if (allowed & ~DEBUG_OPTIONS) {
        fprintf(out, "\nOptions:\n");
        print_option_group(out, allowed & ~DEBUG_OPTIONS);
    }
    if (allowed & DEBUG_OPTIONS) {
        fprintf(out, "\nDebug and development (bypasses safety checks):\n");
        print_option_group(out, allowed & DEBUG_OPTIONS);
    }
}

alltrax_write_opts cli_write_opts(const cli_flags* flags)
{
    return (alltrax_write_opts){
        .skip_cal = flags->debug_no_cal,
        .skip_verify = flags->debug_no_verify,
        .skip_goodset = flags->debug_no_goodset,
        .skip_fw_check = flags->allow_unvalidated_firmware,
        .skip_voltage_link = flags->no_voltage_link,
    };
}

int cli_parse_args(int argc, char** argv, const char* command,
    unsigned allowed, int min_args, int max_args, cli_flags* flags)
{
    memset(flags, 0, sizeof(*flags));

    int npos = 0;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (strncmp(arg, "--", 2) != 0) {
            argv[1 + npos++] = argv[i];
            continue;
        }

        unsigned bit = option_bit(arg);
        if (!bit) {
            fprintf(stderr, "Error: %s: unknown option %s\n", command, arg);
            return -1;
        }
        if (!(bit & allowed)) {
            fprintf(stderr, "Error: %s does not accept %s\n", command, arg);
            return -1;
        }

        if (bit == CLI_OPT_FILE || bit == CLI_OPT_PRESET) {
            const char** value = bit == CLI_OPT_FILE ? &flags->file
                                                     : &flags->preset;
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
        case CLI_OPT_NO_VOLTAGE_LINK:   flags->no_voltage_link = true; break;
        case CLI_OPT_ALLOW_UNVALIDATED: flags->allow_unvalidated_firmware = true; break;
        case CLI_OPT_FIELD_MAP:         flags->field_map = true; break;
        case CLI_OPT_NO_CRYPT:          flags->no_crypt = true; break;
        case CLI_OPT_RESET:             flags->reset = true; break;
        case CLI_OPT_PLOT:              flags->plot = true; break;
        case CLI_OPT_DEBUG_NO_CAL:      flags->debug_no_cal = true; break;
        case CLI_OPT_DEBUG_NO_VERIFY:   flags->debug_no_verify = true; break;
        case CLI_OPT_DEBUG_NO_GOODSET:  flags->debug_no_goodset = true; break;
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
