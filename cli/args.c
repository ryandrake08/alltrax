/*
 * args.c — Shared command-line option parsing
 */

#include <stdio.h>
#include <string.h>
#include "cli.h"

bool cli_no_fw_version = false;

static const struct {
    const char* name;
    unsigned    bit;
} options[] = {
    { "--no-cal",          CLI_OPT_NO_CAL },
    { "--no-verify",       CLI_OPT_NO_VERIFY },
    { "--no-goodset",      CLI_OPT_NO_GOODSET },
    { "--no-fw-version",   CLI_OPT_NO_FW_VERSION },
    { "--no-crypt",        CLI_OPT_NO_CRYPT },
    { "--no-voltage-link", CLI_OPT_NO_VOLTAGE_LINK },
    { "--reset",           CLI_OPT_RESET },
    { "--plot",            CLI_OPT_PLOT },
    { "--file",            CLI_OPT_FILE },
    { "--preset",          CLI_OPT_PRESET },
};

static unsigned option_bit(const char* arg)
{
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        if (strcmp(arg, options[i].name) == 0)
            return options[i].bit;
    }
    return 0;
}

int cli_parse_args(int argc, char** argv, const char* command,
    unsigned allowed, int min_args, int max_args, cli_flags* flags)
{
    memset(flags, 0, sizeof(*flags));
    flags->no_fw_version = cli_no_fw_version;

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
        case CLI_OPT_NO_CAL:          flags->no_cal = true; break;
        case CLI_OPT_NO_VERIFY:       flags->no_verify = true; break;
        case CLI_OPT_NO_GOODSET:      flags->no_goodset = true; break;
        case CLI_OPT_NO_FW_VERSION:   flags->no_fw_version = true; break;
        case CLI_OPT_NO_CRYPT:        flags->no_crypt = true; break;
        case CLI_OPT_NO_VOLTAGE_LINK: flags->no_voltage_link = true; break;
        case CLI_OPT_RESET:           flags->reset = true; break;
        case CLI_OPT_PLOT:            flags->plot = true; break;
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
