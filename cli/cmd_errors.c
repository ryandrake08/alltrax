/*
 * cmd_errors.c — alltrax errors: Error flags and history
 */

#include <stdio.h>
#include "cli.h"

int cmd_errors(int argc, char** argv)
{
    (void)argc; (void)argv;

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;

    alltrax_monitor_data data;
    err = alltrax_read_monitor(ctrl, &data);
    if (err) {
        cli_error(ctrl, err, "reading error data");
        alltrax_close(ctrl);
        return 1;
    }

    /* Active errors */
    printf("Active Errors:\n");
    int any_active = 0;
    for (int i = 0; i < 17; i++) {
        if (data.errors[i]) {
            printf("  %-30s %d\n", alltrax_error_flag_name(i), data.errors[i]);
            any_active = 1;
        }
    }
    if (!any_active)
        printf("  (none)\n");

    /* Error history */
    printf("\nError History:\n");
    int any_history = 0;
    for (int i = 0; i < 17; i++) {
        if (data.error_history[i]) {
            printf("  %-30s %u\n", alltrax_error_flag_name(i),
                   data.error_history[i]);
            any_history = 1;
        }
    }
    if (!any_history)
        printf("  (none)\n");

    alltrax_close(ctrl);
    return 0;
}
