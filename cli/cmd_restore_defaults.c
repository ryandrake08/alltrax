#include "cli.h"

int cmd_restore_defaults(int argc, char** argv)
{
    const cli_option_mask allowed = CLI_OPT_THROTTLE | CLI_OPT_FIELD_MAP |
                                    CLI_OPT_ALLOW_UNVALIDATED;
    cli_flags flags;
    if (cli_parse_args(argc, argv, "restore-defaults", allowed, 0, 0, &flags) < 0) {
        fprintf(stderr, "Usage: alltrax restore-defaults [options]\n");
        cli_print_options(stderr, allowed);
        return 1;
    }

    /* A restore replaces the throttle settings and the field map. */
    if (cli_require_groups("restore-defaults", true, true, &flags)) return 1;

    alltrax_controller* ctrl = NULL;
    if (cli_open(&ctrl, true)) return 1;
    fprintf(stderr, "Warning: restoring the factory field map. Verify it matches the installed motor.\n");

    alltrax_write_report report;
    alltrax_write_opts opts = cli_write_opts(&flags, &report);
    alltrax_error err = alltrax_restore_defaults(ctrl, &opts);
    if (err) {
        cli_error(ctrl, err, "restoring factory defaults");
        if (report.factory_restored)
            fprintf(stderr, "Factory settings were written before the failure; "
                    "reset or power cycle the controller to apply them.\n");
    } else {
        printf("Factory settings restored\n");
        printf("Check that the speed sensor pole count (Tach_4_8) is correct for this motor.\n");
    }
    /* A restore is explicit, so omit the automatic recovery notice. */
    report.factory_restored = false;
    cli_report_write(ctrl, &report);
    alltrax_close(ctrl);
    return err != ALLTRAX_OK;
}
