#include <math.h>
#include <string.h>
#include "cli.h"

bool cli_is_profile_current(const alltrax_var_def* var)
{
    const char* name = var->name;
    if (strncmp(name, "N_", 2) == 0)
        name += 2;
    else if (strncmp(name, "U1_", 3) == 0 || strncmp(name, "U2_", 3) == 0)
        name += 3;
    else
        return false;

    return strcmp(var->unit, "%") == 0 &&
           (strncmp(name, "Max_Arm_", 8) == 0 ||
            strncmp(name, "Max_Batt_", 9) == 0 ||
            strcmp(name, "RollDetect_BrakingCurrent") == 0);
}

int cli_convert_profile_currents(alltrax_controller* ctrl,
    alltrax_assignment* items, const bool* in_amps, size_t count,
    alltrax_info* info, bool* have_info)
{
    *have_info = false;
    for (size_t i = 0; i < count; i++) {
        if (!in_amps[i]) continue;
        if (!*have_info) {
            alltrax_error err = alltrax_get_info(ctrl, info);
            if (err) {
                cli_error(ctrl, err, "reading rated current");
                return 1;
            }
            *have_info = true;
        }

        const alltrax_var_def* var = items[i].var;
        if (!info->rated_amps) {
            fprintf(stderr, "Error: %s requires a known rated current\n", var->name);
            return 1;
        }
        double amps = items[i].number;
        if (amps > info->rated_amps) {
            fprintf(stderr, "Error: %s exceeds the rated current of %u A\n",
                    var->name, (unsigned)info->rated_amps);
            return 1;
        }
        double percent = (amps * 100.0) / info->rated_amps;
        items[i].number = var->raw_max == 0 ? -percent : percent;
    }
    return 0;
}

alltrax_error cli_read_current_settings(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out)
{
    const alltrax_var_def* marker = alltrax_find_var("Var_GoodSet");
    alltrax_var_value value;
    alltrax_error err = alltrax_read_vars(ctrl, &marker, 1, &value);
    if (err) return err;
    return value.raw.u16 != 0 ? alltrax_read_factory_vars(ctrl, vars, count, out)
                              : alltrax_read_vars(ctrl, vars, count, out);
}
