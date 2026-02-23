/*
 * cmd_get.c — alltrax get [var...]: Read settings
 */

#include <stdio.h>
#include <string.h>
#include "cli.h"

static void print_var(const alltrax_var_value* val)
{
    const alltrax_var_def* def = val->def;

    if (def->type == ALLTRAX_TYPE_BOOL) {
        printf("%-30s %s\n", def->name, val->raw.b ? "Yes" : "No");
    } else if (def->type == ALLTRAX_TYPE_STRING) {
        printf("%-30s %s\n", def->name, val->raw.str);
    } else if (def->scale != 1.0 || def->offset != 0) {
        printf("%-30s %.1f %s\n", def->name, val->display,
               def->unit ? def->unit : "");
    } else {
        printf("%-30s %d %s\n", def->name, (int)val->display,
               def->unit ? def->unit : "");
    }
}

int cmd_get(int argc, char** argv)
{
    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;

    if (argc <= 1) {
        /* No args: print all settings groups */
        alltrax_var_group groups[] = {
            ALLTRAX_VARS_VOLTAGE,
            ALLTRAX_VARS_NORMAL_USER,
            ALLTRAX_VARS_USER1,
            ALLTRAX_VARS_USER2,
            ALLTRAX_VARS_THROTTLE,
            ALLTRAX_VARS_OTHER_SETTINGS,
        };
        size_t ngroups = sizeof(groups) / sizeof(groups[0]);

        for (size_t g = 0; g < ngroups; g++) {
            size_t count;
            const alltrax_var_def* defs = alltrax_get_var_defs(groups[g], &count);
            if (!defs || count == 0) continue;

            const alltrax_var_def* ptrs[64];
            for (size_t i = 0; i < count && i < 64; i++)
                ptrs[i] = &defs[i];

            alltrax_var_value vals[64];
            err = alltrax_read_vars(ctrl, ptrs, count, vals);
            if (err) {
                cli_error(ctrl, err, "reading variables");
                alltrax_close(ctrl);
                return 1;
            }

            for (size_t i = 0; i < count; i++)
                print_var(&vals[i]);
        }
    } else {
        /* Specific variables by name */
        for (int i = 1; i < argc; i++) {
            const alltrax_var_def* var = alltrax_find_var(argv[i]);
            if (!var) {
                fprintf(stderr, "Unknown variable: %s\n", argv[i]);
                alltrax_close(ctrl);
                return 1;
            }

            alltrax_var_value val;
            err = alltrax_read_vars(ctrl, &var, 1, &val);
            if (err) {
                cli_error(ctrl, err, argv[i]);
                alltrax_close(ctrl);
                return 1;
            }

            print_var(&val);
        }
    }

    alltrax_close(ctrl);
    return 0;
}
