#include "cli.h"

int cmd_fan_test(int argc, char** argv)
{
    cli_flags flags;
    const cli_option_mask allowed = CLI_OPT_ALLOW_UNVALIDATED;
    if (cli_parse_args(argc, argv, "fan-test", allowed, 0, 0, &flags) < 0) {
        fprintf(stderr, "Usage: alltrax fan-test [options]\n");
        cli_print_options(stderr, allowed);
        return 1;
    }

    alltrax_controller* ctrl = NULL;
    if (cli_open(&ctrl, true)) return 1;

    const alltrax_var_def* keyswitch = alltrax_find_var("Keyswitch_Input");
    alltrax_var_value value;
    alltrax_error err = alltrax_read_vars(ctrl, &keyswitch, 1, &value);
    int rc = 1;
    if (err) {
        cli_error(ctrl, err, "reading the keyswitch");
    } else if (!value.raw.b) {
        fprintf(stderr, "Error: turn the keyswitch on before starting the fan test\n");
    } else {
        const alltrax_var_def* fan = alltrax_find_var("Fan_On");
        const double on = 1;
        alltrax_write_opts opts = cli_write_opts(&flags, NULL);
        err = alltrax_write_vars(ctrl, &fan, &on, 1, &opts);
        if (err) {
            cli_error(ctrl, err, "starting the fan test");
        } else {
            printf("Fan test started\n");
            rc = 0;
        }
    }
    alltrax_close(ctrl);
    return rc;
}
