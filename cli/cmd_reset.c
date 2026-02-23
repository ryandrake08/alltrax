/*
 * cmd_reset.c — alltrax reset: Reboot controller
 */

#include <stdio.h>
#include "cli.h"

int cmd_reset(int argc, char** argv)
{
    (void)argc; (void)argv;

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, true);
    if (err) return 1;

    err = alltrax_reset_device(ctrl);
    if (err) {
        cli_error(ctrl, err, "resetting device");
        alltrax_close(ctrl);
        return 1;
    }

    printf("Device reset sent (controller will reboot)\n");
    alltrax_close(ctrl);
    return 0;
}
