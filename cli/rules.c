/*
 * rules.c — Value rules for settings and curves entered through the CLI
 *
 * The library refuses only what the controller cannot store. These rules
 * refuse values it would store but that are unsafe or meaningless on a
 * vehicle. --skip-rules skips them.
 *
 * Curves:
 *   - Throttle curves (linearization, speed, torque) start at 0,0, end at
 *     full-scale X, step X by at least 1 % of full scale, and never decrease
 *     in Y, so a released pedal gives no output and more pedal never gives
 *     less.
 *   - Field-map curves (field, genfield) start at X 0, end at full-scale X
 *     and strictly increase in X; their Y may fall.
 *   - Trailing 0,0 pairs are unused points.
 * Settings:
 *   - Voltage settings lie in the range for the rated voltage (48 V:
 *     16.0-62.0 V, 72 V: 20.0-90.0 V); other ratings cannot set them.
 *   - Voltage settings keep their order: the KSI threshold at least 1 V below
 *     the low-battery cutoff, which is at least 1 V below both the
 *     high-battery cutoff and the BMS-missing cutoff, which does not exceed
 *     the high-battery cutoff. Settings outside the batch come from the page
 *     the write lands on: the factory page while the user page is invalid.
 *   - The throttle type is one the controller's hardware allows.
 *   - Roll-detect braking current is at least 10 A in magnitude.
 *   - Throttle rate and its analog maximum are at most 100 MU.
 *   - Speed limiting needs a speed sensor.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"

#define MIN_ROLL_DETECT_AMPS 10.0
#define MAX_THROTTLE_RATE    100.0

static bool rules_skipped(const cli_flags* flags)
{
    if (flags->skip_rules)
        fprintf(stderr, "Warning: value rules skipped (--skip-rules)\n");
    return flags->skip_rules;
}

static int check_curve(const alltrax_curve_data* curve)
{
    const alltrax_curve_def* def = curve->def;
    bool field = cli_is_field_map_curve(def);
    long x[ALLTRAX_CURVE_POINTS], y[ALLTRAX_CURVE_POINTS];
    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
        x[i] = lround(curve->x[i] / def->x_scale);
        y[i] = lround(curve->y[i] / def->y_scale);
    }
    int last = ALLTRAX_CURVE_POINTS - 1;
    while (last > 0 && x[last] == 0 && y[last] == 0)
        last--;

    if (x[0] != 0 || (!field && y[0] != 0)) {
        fprintf(stderr, "Error: %s curve must start at %s\n", def->name,
                field ? "X 0" : "0,0");
        return 1;
    }
    if (x[last] != def->x_raw_max) {
        fprintf(stderr, "Error: %s curve must end at X %g %s\n", def->name,
                def->x_raw_max * def->x_scale, def->x_unit);
        return 1;
    }
    long min_step = field ? 1 : def->x_raw_max / 100;
    for (int i = 1; i <= last; i++) {
        if (x[i] - x[i - 1] < min_step) {
            fprintf(stderr, "Error: %s curve point %d: X must increase%s\n",
                    def->name, i + 1, field ? "" : " by at least 1 %");
            return 1;
        }
        if (!field && y[i] < y[i - 1]) {
            fprintf(stderr, "Error: %s curve point %d: Y must not decrease\n",
                    def->name, i + 1);
            return 1;
        }
    }
    return 0;
}

int cli_rules_check_curve(const alltrax_curve_data* curve, const cli_flags* flags)
{
    return rules_skipped(flags) ? 0 : check_curve(curve);
}

/* Profile setting name without its N_, U1_ or U2_ prefix, or NULL. */
static const char* profile_setting(const char* name)
{
    if (strncmp(name, "N_", 2) == 0) return name + 2;
    if (strncmp(name, "U1_", 3) == 0 || strncmp(name, "U2_", 3) == 0) return name + 3;
    return NULL;
}

static int need_info(alltrax_controller* ctrl, alltrax_info* info, bool* have_info)
{
    if (*have_info) return 0;
    alltrax_error err = alltrax_get_info(ctrl, info);
    if (err) {
        cli_error(ctrl, err, "reading controller information");
        return 1;
    }
    *have_info = true;
    return 0;
}

/* Voltage settings in order: KSI, low battery, high battery, BMS missing. */
static const char* const voltage_names[4] = {
    "AnalogInputs_Threshold", "LoBat_Vlim", "HiBat_Vlim", "BMS_Missing_HiBat_Vlim",
};

static int voltage_index(const alltrax_var_def* var)
{
    for (int v = 0; v < 4; v++)
        if (strcmp(var->name, voltage_names[v]) == 0)
            return v;
    return -1;
}

static int check_rated_voltage(const alltrax_var_def* var, double value,
    const alltrax_info* info)
{
    long lo, hi;
    switch (info->rated_voltage) {
    case 48: lo = 160; hi = 620; break;
    case 72: lo = 200; hi = 900; break;
    default:
        fprintf(stderr, "Error: %s: voltage settings are defined only for 48 V "
                "and 72 V controllers (rated %u V)\n", var->name, info->rated_voltage);
        return 1;
    }
    long raw = lround(value / var->scale);
    if (raw < lo || raw > hi) {
        fprintf(stderr, "Error: %s %.1f V is outside %.1f-%.1f V for a %u V controller\n",
                var->name, value, lo * var->scale, hi * var->scale, info->rated_voltage);
        return 1;
    }
    return 0;
}

static int check_throttle_type(long type, const alltrax_info* info)
{
    /* Type 0 (none) needs no hardware; types 1-11 map to bits 0-10. */
    if (type < 1 || type > 11 || (info->throttles_allowed & (UINT32_C(1) << (type - 1))))
        return 0;
    const char* name = alltrax_throttle_type_name((uint8_t)type);
    fprintf(stderr, "Error: throttle type %ld (%s) is not supported by this "
            "controller (allowed: 0x%04X)\n", type, name ? name : "unknown",
            (unsigned)info->throttles_allowed);
    return 1;
}

static int check_setting(alltrax_controller* ctrl, const alltrax_assignment* item,
    alltrax_info* info, bool* have_info)
{
    const alltrax_var_def* var = item->var;
    const char* setting = profile_setting(var->name);
    double value = item->number;

    if (voltage_index(var) >= 0) {
        if (need_info(ctrl, info, have_info)) return 1;
        if (check_rated_voltage(var, value, info)) return 1;
    } else if (strcmp(var->name, "Throttle_Type") == 0 && value != 0) {
        if (need_info(ctrl, info, have_info)) return 1;
        if (check_throttle_type(lround(value), info)) return 1;
    } else if (setting && strcmp(setting, "RollDetect_BrakingCurrent") == 0) {
        if (need_info(ctrl, info, have_info)) return 1;
        if (!info->rated_amps) {
            fprintf(stderr, "Error: %s requires a known rated current\n", var->name);
            return 1;
        }
        /* Compare stored magnitudes: 4095 is the rated current. */
        long raw = labs(lround(value / var->scale));
        long min_raw = lround(4095.0 * MIN_ROLL_DETECT_AMPS / info->rated_amps);
        if (raw < min_raw) {
            fprintf(stderr, "Error: %s must be at least %g A (%.1f A requested)\n",
                    var->name, MIN_ROLL_DETECT_AMPS, raw * info->rated_amps / 4095.0);
            return 1;
        }
    } else if (setting && (strcmp(setting, "Throttle_Rate") == 0 ||
                           strcmp(setting, "Throttle_Rate_Max") == 0)) {
        if (value > MAX_THROTTLE_RATE) {
            fprintf(stderr, "Error: %s must be at most %g %s\n",
                    var->name, MAX_THROTTLE_RATE, var->unit);
            return 1;
        }
    } else if (strcmp(var->name, "Speed_Limit_On") == 0 && value != 0) {
        if (need_info(ctrl, info, have_info)) return 1;
        if (!info->speed_sensor) {
            fprintf(stderr, "Error: Speed_Limit_On requires a speed sensor\n");
            return 1;
        }
    }
    return 0;
}

/* Voltage settings in the batch, with the others read from the page the
 * write lands on, in stored units (0.1 V). */
static int check_voltage_order(alltrax_controller* ctrl,
    const alltrax_assignment* items, size_t count)
{
    long volts[4];
    bool given[4] = { false, false, false, false };
    bool any = false;
    for (size_t i = 0; i < count; i++) {
        const alltrax_var_def* var = items[i].var;
        int v = var && !items[i].curve ? voltage_index(var) : -1;
        if (v < 0 || alltrax_validate_var_value(var, items[i].number)) continue;
        volts[v] = lround(items[i].number / var->scale);
        given[v] = any = true;
    }
    if (!any) return 0;

    const alltrax_var_def* missing[4];
    int index[4];
    size_t n = 0;
    for (int v = 0; v < 4; v++) {
        if (!given[v]) {
            missing[n] = alltrax_find_var(voltage_names[v]);
            index[n++] = v;
        }
    }
    if (n) {
        alltrax_var_value values[4];
        alltrax_error err = cli_read_current_settings(ctrl, missing, n, values);
        if (err) {
            cli_error(ctrl, err, "reading voltage settings");
            return 1;
        }
        for (size_t i = 0; i < n; i++)
            volts[index[i]] = lround(values[i].display / missing[i]->scale);
    }

    const long gap = 10;  /* 1 V */
    const struct { int lower, upper; long min_gap; } pairs[] = {
        { 0, 1, gap }, { 1, 2, gap }, { 1, 3, gap }, { 3, 2, 0 },
    };
    for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
        long lower = volts[pairs[p].lower], upper = volts[pairs[p].upper];
        if (upper - lower < pairs[p].min_gap) {
            fprintf(stderr, "Error: %s (%.1f V) must be %s %s (%.1f V)\n",
                    voltage_names[pairs[p].lower], lower / 10.0,
                    pairs[p].min_gap ? "at least 1 V below" : "at most",
                    voltage_names[pairs[p].upper], upper / 10.0);
            return 1;
        }
    }
    return 0;
}

int cli_rules_check(alltrax_controller* ctrl, const alltrax_assignment* items,
    size_t count, const alltrax_info* info, const cli_flags* flags)
{
    if (rules_skipped(flags)) return 0;
    alltrax_info read_info;
    bool have_info = info != NULL;
    if (info) read_info = *info;
    for (size_t i = 0; i < count; i++) {
        if (items[i].curve) {
            if (check_curve(items[i].curve_data)) return 1;
        } else if (items[i].var && items[i].var->type != ALLTRAX_TYPE_STRING) {
            /* Values the controller cannot store are refused by the write
             * itself, before any device I/O. */
            if (alltrax_validate_var_value(items[i].var, items[i].number)) continue;
            if (check_setting(ctrl, &items[i], &read_info, &have_info)) return 1;
        }
    }
    return check_voltage_order(ctrl, items, count);
}
