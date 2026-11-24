#include <inttypes.h>
#include <string.h>
#include "cli.h"

int cmd_field_map(int argc, char** argv)
{
    cli_flags flags;
    if (argc < 2 || strcmp(argv[1], "history") != 0 ||
        cli_parse_args(argc - 1, argv + 1, "field-map history", 0, 0, 0, &flags) < 0) {
        fprintf(stderr, "Usage: alltrax field-map history\n");
        return 1;
    }

    alltrax_controller* ctrl = NULL;
    if (cli_open(&ctrl, false)) return 1;
    alltrax_field_map_history history;
    alltrax_error err = alltrax_read_field_map_history(ctrl, &history);
    if (err) {
        cli_error(ctrl, err, "reading field-map history");
        alltrax_close(ctrl);
        return 1;
    }
    alltrax_close(ctrl);

    printf("Map count: %" PRIu32 "\n", history.map_count);
    for (size_t i = 0; i < history.record_count; i++) {
        const alltrax_field_map_history_record* record = &history.records[i];
        printf("%u/%u/%04u  %s\n", (unsigned)record->month, (unsigned)record->day,
               (unsigned)record->year, record->name);
    }
    return 0;
}
