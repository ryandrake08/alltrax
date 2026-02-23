/*
 * cmd_info.c — alltrax info: Controller identity and firmware
 */

#include <stdio.h>
#include "cli.h"

int cmd_info(int argc, char** argv)
{
    (void)argc; (void)argv;

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;

    alltrax_info info;
    err = alltrax_get_info(ctrl, &info);
    if (err) {
        cli_error(ctrl, err, "reading controller info");
        alltrax_close(ctrl);
        return 1;
    }

    char rev_buf[16];

    printf("Model:            %s\n", info.model);
    printf("Build Date:       %s\n", info.build_date);
    printf("Serial Number:    %u\n", info.serial_number);
    printf("Boot Rev:         %s\n",
           alltrax_format_rev(info.boot_rev, rev_buf, sizeof(rev_buf)));
    printf("Original Boot:    %s\n",
           alltrax_format_rev(info.original_boot_rev, rev_buf, sizeof(rev_buf)));
    printf("Program Rev:      %s\n",
           alltrax_format_rev(info.program_rev, rev_buf, sizeof(rev_buf)));
    printf("Original Program: %s\n",
           alltrax_format_rev(info.original_program_rev, rev_buf, sizeof(rev_buf)));
    printf("Program Version:  %u\n", info.program_ver);
    printf("Rated Voltage:    %uV\n", info.rated_voltage);
    printf("Rated Current:    %uA\n", info.rated_amps);

    alltrax_close(ctrl);
    return 0;
}
