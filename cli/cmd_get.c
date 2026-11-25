/*
 * cmd_get.c — alltrax get [var...]: Read settings
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cli.h"

static int print_var(alltrax_controller* ctrl, const alltrax_var_value* val,
    alltrax_info* info, bool* have_info)
{
    const alltrax_var_def* def = val->def;

    if (def->type == ALLTRAX_TYPE_BOOL) {
        printf("%-30s %s\n", def->name, val->raw.b ? "Yes" : "No");
    } else if (def->type == ALLTRAX_TYPE_STRING) {
        printf("%-30s %s\n", def->name, val->raw.str);
    } else if (cli_is_profile_current(def)) {
        if (!*have_info) {
            alltrax_error err = alltrax_get_info(ctrl, info);
            if (err) {
                cli_error(ctrl, err, "reading rated current");
                return 1;
            }
            *have_info = true;
        }
        printf("%-30s %.1f %%", def->name, val->display);
        if (info->rated_amps)
            printf(" (%.1f A)", fabs(val->display) * info->rated_amps / 100.0);
        printf("\n");
    } else if (def->scale != 1.0 || def->offset != 0) {
        printf("%-30s %.1f %s\n", def->name, val->display,
               def->unit ? def->unit : "");
    } else {
        printf("%-30s %" PRId64 " %s\n", def->name,
               alltrax_var_raw_int64(val), def->unit ? def->unit : "");
    }
    return 0;
}

static int check_settings_validity(alltrax_controller* ctrl, bool factory)
{
    const alltrax_var_def* marker = alltrax_find_var(factory ? "Fact_GoodSet" : "Var_GoodSet");
    alltrax_var_value value;
    alltrax_error err = alltrax_read_vars(ctrl, &marker, 1, &value);
    if (err) {
        cli_error(ctrl, err, "reading settings validity");
        return 1;
    }
    if (value.raw.u16 == 0) return 0;

    if (factory) {
        fprintf(stderr, "Error: factory settings are invalid (Fact_GoodSet=0x%04X)\n",
                (unsigned)value.raw.u16);
        return 1;
    }
    unsigned user_goodset = value.raw.u16;
    marker = alltrax_find_var("Fact_GoodSet");
    err = alltrax_read_vars(ctrl, &marker, 1, &value);
    if (err) {
        cli_error(ctrl, err, "reading factory settings validity");
        return 1;
    }
    if (value.raw.u16 == 0) {
        fprintf(stderr, "Warning: user settings are invalid (Var_GoodSet=0x%04X). "
                        "Use get --factory to read factory settings.\n", user_goodset);
    } else {
        fprintf(stderr, "Warning: user and factory settings are invalid "
                        "(Var_GoodSet=0x%04X, Fact_GoodSet=0x%04X).\n",
                user_goodset, (unsigned)value.raw.u16);
    }
    return 0;
}

int cmd_get(int argc, char** argv)
{
    cli_flags flags;
    int nargs = cli_parse_args(argc, argv, "get", CLI_OPT_FACTORY, 0, -1, &flags);
    if (nargs < 0) {
        fprintf(stderr, "Usage: alltrax get [options] [var...]\n");
        cli_print_options(stderr, CLI_OPT_FACTORY);
        return 1;
    }
    for (int i = 1; i <= nargs; i++) {
        if (!alltrax_find_var(argv[i])) {
            fprintf(stderr, "Unknown variable: %s\n", argv[i]);
            return 1;
        }
    }

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;
    alltrax_info info;
    bool have_info = false;
    bool checked_goodset = false;
    int ret = 1;

    if (nargs == 0) {
        if (check_settings_validity(ctrl, flags.factory))
            goto done;
        /* No args: print the writable settings from the selected page. */
        alltrax_var_group groups[] = {
            ALLTRAX_VARS_VOLTAGE,
            ALLTRAX_VARS_NORMAL_USER,
            ALLTRAX_VARS_USER1,
            ALLTRAX_VARS_USER2,
            ALLTRAX_VARS_TACH,
            ALLTRAX_VARS_OTHER_SETTINGS,
            ALLTRAX_VARS_THROTTLE,
        };
        size_t ngroups = sizeof(groups) / sizeof(groups[0]);

        for (size_t g = 0; g < ngroups; g++) {
            size_t count;
            const alltrax_var_def* defs = alltrax_get_var_defs(groups[g], &count);
            if (!defs || count == 0) continue;

            const alltrax_var_def* ptrs[64];
            size_t n = 0;
            for (size_t i = 0; i < count && n < 64; i++) {
                if (defs[i].writable)
                    ptrs[n++] = &defs[i];
            }

            alltrax_var_value vals[64];
            err = flags.factory ? alltrax_read_factory_vars(ctrl, ptrs, n, vals)
                                : alltrax_read_vars(ctrl, ptrs, n, vals);
            if (err) {
                cli_error(ctrl, err, "reading variables");
                goto done;
            }

            for (size_t i = 0; i < n; i++)
                if (print_var(ctrl, &vals[i], &info, &have_info))
                    goto done;
        }
    } else {
        /* Specific variables by name */
        for (int i = 1; i <= nargs; i++) {
            const alltrax_var_def* var = alltrax_find_var(argv[i]);

            /* Settings (writable FLASH entries) depend on the page markers. */
            if (!checked_goodset && var->is_flash && var->writable) {
                if (check_settings_validity(ctrl, flags.factory)) goto done;
                checked_goodset = true;
            }

            alltrax_var_value val;
            err = flags.factory ? alltrax_read_factory_vars(ctrl, &var, 1, &val)
                                : alltrax_read_vars(ctrl, &var, 1, &val);
            if (err) {
                cli_error(ctrl, err, argv[i]);
                goto done;
            }

            if (print_var(ctrl, &val, &info, &have_info))
                goto done;
        }
    }

    ret = 0;
done:
    alltrax_close(ctrl);
    return ret;
}
