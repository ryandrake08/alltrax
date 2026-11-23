/*
 * cmd_reset.c — alltrax reset: Reboot controller
 */

#include <stdio.h>
#include "cli.h"

int cmd_reset(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "reset", CLI_OPT_ALLOW_UNVALIDATED, 0, 0,
                       &flags) < 0) {
        fprintf(stderr, "Usage: alltrax reset [options]\n");
        cli_print_options(stderr, CLI_OPT_ALLOW_UNVALIDATED);
        return 1;
    }
    alltrax_write_opts opts = cli_write_opts(&flags);

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, true);
    if (err) return 1;

    err = alltrax_reset_device(ctrl, &opts);
    if (err) {
        cli_error(ctrl, err, "resetting device");
        alltrax_close(ctrl);
        return 1;
    }

    printf("Device reset sent (controller will reboot)\n");
    alltrax_close(ctrl);
    return 0;
}
