/*
 * main.c — CLI entry point and subcommand dispatch
 */

#include <stdio.h>
#include <string.h>
#include "cli.h"

alltrax_error cli_open(alltrax_controller** ctrl, bool writes)
{
    alltrax_error err = alltrax_open(ctrl, writes);
    if (err) {
        fprintf(stderr, "Error: %s\n", alltrax_strerror(err));
        if (*ctrl)
            fprintf(stderr, "  %s\n", alltrax_last_error_detail(*ctrl));
    }
    return err;
}

void cli_error(alltrax_controller* ctrl, alltrax_error err, const char* action)
{
    fprintf(stderr, "Error: %s: %s\n", action, alltrax_strerror(err));
    if (ctrl) {
        const char* detail = alltrax_last_error_detail(ctrl);
        if (detail[0])
            fprintf(stderr, "  %s\n", detail);
    }
}

int cli_parse_flags(int argc, char** argv, cli_flags* flags)
{
    memset(flags, 0, sizeof(*flags));
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-cal") == 0)
            flags->no_cal = true;
        else if (strcmp(argv[i], "--no-verify") == 0)
            flags->no_verify = true;
        else if (strcmp(argv[i], "--force") == 0)
            flags->force = true;
        else
            break;  /* First non-flag arg */
    }
    return i;
}

static void print_usage(void)
{
    fprintf(stderr,
        "Usage: alltrax <command> [options]\n"
        "\n"
        "Commands:\n"
        "  info      Controller identity and firmware\n"
        "  get       Read settings (all or by name)\n"
        "  write     Write settings (var=value ...)\n"
        "  reset     Reboot controller\n"
        "  monitor   Live polling (Ctrl+C to stop)\n"
        "  errors    Error flags and history\n"
        "\n"
        "Global flags:\n"
        "  --no-cal      Skip CAL/RUN bracket\n"
        "  --no-verify   Skip read-back verification\n"
        "  --force       Allow writes on unrecognized firmware\n"
    );
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "info") == 0)
        return cmd_info(argc - 1, argv + 1);
    if (strcmp(cmd, "get") == 0)
        return cmd_get(argc - 1, argv + 1);
    if (strcmp(cmd, "write") == 0)
        return cmd_write(argc - 1, argv + 1);
    if (strcmp(cmd, "reset") == 0)
        return cmd_reset(argc - 1, argv + 1);
    if (strcmp(cmd, "monitor") == 0)
        return cmd_monitor(argc - 1, argv + 1);
    if (strcmp(cmd, "errors") == 0)
        return cmd_errors(argc - 1, argv + 1);

    fprintf(stderr, "Unknown command: %s\n", cmd);
    print_usage();
    return 1;
}
