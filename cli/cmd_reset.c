/*
 * cmd_reset.c — alltrax reset: Reboot controller
 */

#include <stdio.h>
#include "cli.h"

int cmd_reset(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "reset", CLI_OPT_NO_FW_VERSION, 0, 0,
                       &flags) < 0)
        return 1;
    alltrax_write_opts opts = { .skip_fw_check = flags.no_fw_version };

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
