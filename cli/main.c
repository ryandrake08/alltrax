/*
 * main.c — CLI entry point and subcommand dispatch
 */

#include <stdio.h>
#include <string.h>
#include "cli.h"

const char* cli_device_path = NULL;

alltrax_error cli_open(alltrax_controller** ctrl, bool writes)
{
    alltrax_error err = alltrax_open(ctrl, cli_device_path, writes);
    if (err) {
        fprintf(stderr, "Error: %s\n", alltrax_strerror(err));
        if (*ctrl)
            fprintf(stderr, "  %s\n", alltrax_last_error_detail(*ctrl));
        return err;
    }
    if (cli_ignore_fw_bounds)
        alltrax_set_firmware_read_override(*ctrl, true);
    return ALLTRAX_OK;
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

static void print_usage(void)
{
    fprintf(stderr,
        "Usage: alltrax [global options] <command> [options]\n"
        "\n"
        "Global options:\n"
        "  --device <path>           HID device path (e.g. /dev/hidraw0)\n"
        "\n"
        "Debug and development (bypasses safety checks):\n"
        "  --debug-ignore-fw-bounds  Read firmware outside V0.001-V5.999\n"
        "\n"
        "Commands:\n"
        "  info           Controller identity and firmware\n"
        "  devices        List controllers by path, model and serial number\n"
        "  get            Read settings (all or by name)\n"
        "  write          Write settings (var=value ...)\n"
        "  reset          Reboot controller\n"
        "  clear-autocal  Clear throttle calibration and reboot\n"
        "  fan-test       Start a timed fan test (keyswitch must be on)\n"
        "  monitor        Live polling (Ctrl+C to stop)\n"
        "  errors         Error flags and history\n"
        "  config         Save/load config files\n"
        "  curve          Read/write curve tables\n"
    );
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    /* Parse global flags before the command */
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--device") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "Error: --device requires a path argument\n");
                return 1;
            }
            cli_device_path = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--debug-ignore-fw-bounds") == 0) {
            cli_ignore_fw_bounds = true;
            argi++;
        } else {
            break;  /* Not a global flag — let subcommand handle it */
        }
    }

    if (argi >= argc) {
        print_usage();
        return 1;
    }

    alltrax_error err = alltrax_init();
    if (err) {
        fprintf(stderr, "Error: %s\n", alltrax_strerror(err));
        return 1;
    }

    const char* cmd = argv[argi];
    int sub_argc = argc - argi;
    char** sub_argv = argv + argi;
    int rc;

    if (strcmp(cmd, "info") == 0)
        rc = cmd_info(sub_argc, sub_argv);
    else if (strcmp(cmd, "devices") == 0)
        rc = cmd_devices(sub_argc, sub_argv);
    else if (strcmp(cmd, "get") == 0)
        rc = cmd_get(sub_argc, sub_argv);
    else if (strcmp(cmd, "write") == 0)
        rc = cmd_write(sub_argc, sub_argv);
    else if (strcmp(cmd, "reset") == 0)
        rc = cmd_reset(sub_argc, sub_argv);
    else if (strcmp(cmd, "clear-autocal") == 0)
        rc = cmd_clear_autocal(sub_argc, sub_argv);
    else if (strcmp(cmd, "fan-test") == 0)
        rc = cmd_fan_test(sub_argc, sub_argv);
    else if (strcmp(cmd, "monitor") == 0)
        rc = cmd_monitor(sub_argc, sub_argv);
    else if (strcmp(cmd, "errors") == 0)
        rc = cmd_errors(sub_argc, sub_argv);
    else if (strcmp(cmd, "config") == 0)
        rc = cmd_config(sub_argc, sub_argv);
    else if (strcmp(cmd, "curve") == 0)
        rc = cmd_curve(sub_argc, sub_argv);
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage();
        rc = 1;
    }

    alltrax_exit();
    return rc;
}
