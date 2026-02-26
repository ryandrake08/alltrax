/*
 * cmd_info.c — alltrax info: Controller identity and firmware
 */

#include <stdio.h>
#include "cli.h"

static const char* yn(bool v) { return v ? "Yes" : "No"; }

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

    printf("Controller Type:  %s\n",
           alltrax_controller_type_name(info.controller_type));
    printf("Model:            %s\n", info.model);
    printf("Build Date:       %s\n", info.build_date);
    printf("Serial Number:    %u\n", info.serial_number);
    printf("Boot Rev:         %s\n", info.boot_rev_str);
    printf("Original Boot:    %s\n", info.original_boot_rev_str);
    printf("Program Rev:      %s\n", info.program_rev_str);
    printf("Original Program: %s\n", info.original_program_rev_str);
    printf("Program Type:     %u\n", info.program_type);
    printf("Program Version:  %u\n", info.program_ver);
    printf("Hardware Rev:     %u\n", info.hardware_rev);

    printf("\nRatings:\n");
    printf("  Voltage:        %uV\n", info.rated_voltage);
    printf("  Current:        %uA\n", info.rated_amps);
    printf("  Field Current:  %uA\n", info.rated_field_amps);

    printf("\nHardware:\n");
    printf("  Speed Sensor:   %s\n", yn(info.speed_sensor));
    printf("  BMS CAN:        %s\n", yn(info.has_bms_can));
    printf("  Throttle CAN:   %s\n", yn(info.has_throt_can));
    printf("  Forward Input:  %s\n", yn(info.has_forward));
    printf("  User 1 Input:   %s\n", yn(info.has_user1));
    printf("  User 2 Input:   %s\n", yn(info.has_user2));
    printf("  User 3 Input:   %s\n", yn(info.has_user3));
    printf("  Aux Out 1:      %s\n", yn(info.has_aux_out1));
    printf("  Aux Out 2:      %s\n", yn(info.has_aux_out2));
    printf("  High Side:      %s\n", yn(info.can_high_side));
    printf("  Stock Mode:     %s\n", yn(info.is_stock_mode));
    printf("  Throttles:      0x%04X\n", info.throttles_allowed);

    if (!info.supported) {
        printf("\nWarning: %s controllers are not supported.\n",
               alltrax_controller_type_name(info.controller_type));
        printf("Only XCT controllers with firmware V0.001-V5.999 are "
               "tested and validated.\n");
        printf("Read/write operations may produce incorrect results.\n");
    }

    alltrax_close(ctrl);
    return 0;
}
