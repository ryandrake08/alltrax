/*
 * cmd_reset.c — alltrax reset: Reboot controller
 */

#include <stdio.h>
#include "cli.h"

static int reboot(int argc, char** argv, bool clear_autocal)
{
    const char* command = clear_autocal ? "clear-autocal" : "reset";
    const cli_option_mask allowed = CLI_OPT_ALLOW_UNVALIDATED;
    cli_flags flags;
    if (cli_parse_args(argc, argv, command, allowed, 0, 0, &flags) < 0) {
        fprintf(stderr, "Usage: alltrax %s [options]\n", command);
        cli_print_options(stderr, allowed);
        return 1;
    }
    alltrax_write_opts opts = cli_write_opts(&flags, NULL);

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, true);
    if (err) return 1;

    err = clear_autocal ? alltrax_clear_autocal(ctrl, &opts)
                        : alltrax_reset_device(ctrl, &opts);
    if (err) {
        cli_error(ctrl, err, clear_autocal ? "clearing throttle calibration and resetting"
                                         : "resetting device");
        alltrax_close(ctrl);
        return 1;
    }

    printf("Device reset sent (controller will reboot)\n");
    cli_wait_for_reconnect(ctrl);
    alltrax_close(ctrl);
    return 0;
}

int cmd_reset(int argc, char** argv)
{
    return reboot(argc, argv, false);
}

int cmd_clear_autocal(int argc, char** argv)
{
    return reboot(argc, argv, true);
}
