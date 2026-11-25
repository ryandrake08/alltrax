/*
 * field_map.c — Throttle and field-map write guard
 *
 * Throttle settings must match the installed throttle, and the field map the
 * installed motor; a wrong map can damage it. Commands that name what they
 * write need no confirmation. Bulk commands (config load, curve reset all,
 * restore-defaults) write these groups only with --throttle and --field-map.
 * Field-map writes print the motor-match warning and always include
 * F_Table_Name.
 */

#include <stdio.h>
#include <string.h>
#include "cli.h"

bool cli_is_field_map_var(const alltrax_var_def* var)
{
    size_t count;
    const alltrax_var_def* defs = alltrax_get_var_defs(ALLTRAX_VARS_FIELD, &count);
    return var && var->writable && defs && var >= defs && var < defs + count;
}

bool cli_is_field_map_curve(const alltrax_curve_def* def)
{
    return def && (def == alltrax_find_curve("field") ||
                   def == alltrax_find_curve("genfield"));
}

bool cli_assignment_is_field_map(const alltrax_assignment* item)
{
    return cli_is_field_map_var(item->var) || cli_is_field_map_curve(item->curve);
}

bool cli_assignment_is_throttle(const alltrax_assignment* item)
{
    if (item->curve)
        return item->curve == alltrax_find_curve("linearization") ||
               item->curve == alltrax_find_curve("speed") ||
               item->curve == alltrax_find_curve("torque");
    size_t count;
    const alltrax_var_def* defs = alltrax_get_var_defs(ALLTRAX_VARS_THROTTLE, &count);
    return item->var && item->var->writable && defs && item->var >= defs &&
           item->var < defs + count;
}

static bool touches_field_map(const alltrax_assignment* items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (cli_assignment_is_field_map(&items[i]))
            return true;
    }
    return false;
}

int cli_require_groups(const char* what, bool throttle, bool field_map,
    const cli_flags* flags)
{
    bool need_throttle = throttle && !flags->throttle;
    bool need_field_map = field_map && !flags->field_map;
    if (!need_throttle && !need_field_map)
        return 0;
    if (need_throttle && need_field_map)
        fprintf(stderr, "Error: %s includes throttle and field-map settings, which must "
                "match the installed\nthrottle and motor (a wrong field map can damage "
                "the motor). Add --throttle --field-map\nto write them.\n", what);
    else if (need_throttle)
        fprintf(stderr, "Error: %s includes throttle settings, which must match the "
                "installed throttle.\nAdd --throttle to write them.\n", what);
    else
        fprintf(stderr, "Error: %s includes field-map settings, which must match the "
                "installed motor;\na wrong map can damage it. Add --field-map to "
                "write them.\n", what);
    return 1;
}

int cli_check_bulk(const char* what, const alltrax_assignment* items, size_t count,
    const cli_flags* flags)
{
    bool throttle = false;
    for (size_t i = 0; i < count; i++)
        throttle |= cli_assignment_is_throttle(&items[i]);
    return cli_require_groups(what, throttle, touches_field_map(items, count), flags);
}

void cli_warn_field_map(const alltrax_assignment* items, size_t count)
{
    if (touches_field_map(items, count))
        fprintf(stderr,
            "Warning: writing field-map data. It must match the installed motor "
            "(also check\nthe speed sensor pole count, Tach_4_8).\n");
}

alltrax_error cli_add_field_map_name(alltrax_controller* ctrl,
    alltrax_assignment* items, size_t* count, char name[33])
{
    const alltrax_var_def* var = alltrax_find_var("F_Table_Name");
    if (!touches_field_map(items, *count))
        return ALLTRAX_OK;
    for (size_t i = 0; i < *count; i++) {
        if (items[i].var == var)
            return ALLTRAX_OK;
    }
    alltrax_var_value value;
    alltrax_error err = cli_read_current_settings(ctrl, &var, 1, &value);
    if (err) {
        cli_error(ctrl, err, "reading F_Table_Name");
        return err;
    }
    snprintf(name, 33, "%s", value.raw.str);
    items[(*count)++] = (alltrax_assignment){ .var = var, .text = name };
    return ALLTRAX_OK;
}

void cli_field_map_written(const alltrax_assignment* items, size_t count)
{
    if (touches_field_map(items, count))
        printf("Check that the speed sensor pole count (Tach_4_8) is correct "
               "for this motor.\n");
}
