/*
 * cmd_errors.c — alltrax errors: Error flags and history
 */

#include <stdio.h>
#include "cli.h"

#define HISTORY_HOLD_MS 9999

int cmd_errors(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "errors", 0, 0, 0, &flags) < 0)
        return 1;

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

    /* With the keyswitch off the flags and history are not meaningful. */
    if (!data.keyswitch) {
        printf("Errors unavailable with keyswitch off\n");
        alltrax_close(ctrl);
        return 0;
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

    /* Each history entry holds the milliseconds left of a 10-second window:
     * the firmware sets it to HISTORY_HOLD_MS while the error is active and
     * counts it down by 1 per millisecond after the error clears. */
    printf("\nError History (last 10 seconds):\n");
    int any_history = 0;
    for (int i = 0; i < 17; i++) {
        unsigned left = data.error_history[i];
        if (left) {
            unsigned ago = left < HISTORY_HOLD_MS ? HISTORY_HOLD_MS - left : 0;
            printf("  %-30s %.1f s ago\n", alltrax_error_flag_name(i), ago / 1000.0);
            any_history = 1;
        }
    }
    if (!any_history)
        printf("  (none)\n");

    alltrax_close(ctrl);
    return 0;
}
