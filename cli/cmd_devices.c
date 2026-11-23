#include <inttypes.h>
#include "cli.h"

static void print_device(const char* path, const char* model, uint32_t serial, void* user)
{
    (void)user;
    printf("%s\t%s\t%" PRIu32 "\n", path, model, serial);
}

int cmd_devices(int argc, char** argv)
{
    cli_flags flags;
    if (cli_parse_args(argc, argv, "devices", 0, 0, 0, &flags) < 0)
        return 1;
    if (!alltrax_list_devices(cli_device_path, print_device, NULL)) {
        fprintf(stderr, "No responsive Alltrax XCT-family controllers found\n");
        return 1;
    }
    return 0;
}
