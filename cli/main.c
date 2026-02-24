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
        else if (strcmp(argv[i], "--no-goodset") == 0)
            flags->no_goodset = true;
        else if (strcmp(argv[i], "--no-fw-version") == 0)
            flags->no_fw_version = true;
        else if (strcmp(argv[i], "--no-crypt") == 0)
            flags->no_crypt = true;
        else if (strcmp(argv[i], "--reset") == 0)
            flags->reset = true;
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
        "  config    Save/load config files\n"
    );
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    alltrax_error err = alltrax_init();
    if (err) {
        fprintf(stderr, "Error: %s\n", alltrax_strerror(err));
        return 1;
    }

    const char* cmd = argv[1];
    int rc;

    if (strcmp(cmd, "info") == 0)
        rc = cmd_info(argc - 1, argv + 1);
    else if (strcmp(cmd, "get") == 0)
        rc = cmd_get(argc - 1, argv + 1);
    else if (strcmp(cmd, "write") == 0)
        rc = cmd_write(argc - 1, argv + 1);
    else if (strcmp(cmd, "reset") == 0)
        rc = cmd_reset(argc - 1, argv + 1);
    else if (strcmp(cmd, "monitor") == 0)
        rc = cmd_monitor(argc - 1, argv + 1);
    else if (strcmp(cmd, "errors") == 0)
        rc = cmd_errors(argc - 1, argv + 1);
    else if (strcmp(cmd, "config") == 0)
        rc = cmd_config(argc - 1, argv + 1);
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage();
        rc = 1;
    }

    alltrax_exit();
    return rc;
}
