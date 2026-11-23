/*
 * field_map.c — Field-map write guard
 *
 * A field map must match the installed motor; a wrong map can damage it.
 * Writes that touch field-map data require --field-map, print the warning,
 * and always include F_Table_Name.
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

static bool touches_field_map(const alltrax_assignment* items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (cli_assignment_is_field_map(&items[i]))
            return true;
    }
    return false;
}

int cli_check_field_map(const alltrax_assignment* items, size_t count,
    bool allowed)
{
    if (!touches_field_map(items, count))
        return 0;
    if (!allowed) {
        fprintf(stderr,
            "Error: this writes field-map data. The field map must match the "
            "installed motor;\na wrong map can damage it. Add --field-map to "
            "write it.\n");
        return 1;
    }
    fprintf(stderr,
        "Warning: writing field-map data. It must match the installed motor "
        "(also check\nthe speed sensor pole count, Tach_4_8).\n");
    return 0;
}

alltrax_error cli_add_field_map_name(alltrax_controller* ctrl,
    alltrax_assignment* items, size_t* count, char name[33], bool skip_goodset)
{
    const alltrax_var_def* var = alltrax_find_var("F_Table_Name");
    if (!touches_field_map(items, *count))
        return ALLTRAX_OK;
    for (size_t i = 0; i < *count; i++) {
        if (items[i].var == var)
            return ALLTRAX_OK;
    }
    alltrax_var_value value;
    const alltrax_var_def* source = var;
    if (!skip_goodset) {
        const alltrax_var_def* goodset = alltrax_find_var("Var_GoodSet");
        alltrax_error err = alltrax_read_vars(ctrl, &goodset, 1, &value);
        if (err) {
            cli_error(ctrl, err, "reading Var_GoodSet");
            return err;
        }
        if (value.raw.u16 != 0)
            source = alltrax_find_var("F_F_Table_Name");
    }
    alltrax_error err = alltrax_read_vars(ctrl, &source, 1, &value);
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
