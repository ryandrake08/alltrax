#include <string.h>
#include <strings.h>
#include "cli.h"

typedef struct {
    const char* label;
    uint8_t type;
    bool xct_only;
} throttle_entry;

/* Menu labels and their wire types. SRX shares the generic entries and EZGO. */
static const throttle_entry catalog[] = {
    { "EZGO ITS",              4, false },
    { "Club Car IQ/i2 (MCOR)",  5, true  },
    { "Club Car PD+ (V-Glide)", 7, true  },
    { "Yamaha G19/G22",        5, true  },
    { "Yamaha YDRE",           3, true  },
    { "Taylor Dunn",           6, true  },
    { "0-5V",                  3, false },
    { "0-5K 2 Wire",           1, false },
    { "5K-0 2 Wire",           2, false },
    { "0-5K 3 Wire",           5, false },
    { "5K-0 3 Wire",           7, false },
    { "Pump",                  9, false },
    { "USB",                  10, false },
    { "Absolute",             11, false },
};
#define CATALOG_COUNT (sizeof(catalog) / sizeof(catalog[0]))

static const throttle_entry* find_throttle(const char* name)
{
    const throttle_entry* match = NULL;
    size_t len = strlen(name);
    bool ambiguous = false;
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (strcasecmp(name, catalog[i].label) == 0)
            return &catalog[i];
        if (len && strncasecmp(name, catalog[i].label, len) == 0) {
            ambiguous |= match != NULL;
            match = &catalog[i];
        }
    }
    if (ambiguous) {
        fprintf(stderr, "Error: ambiguous throttle name '%s'. Use a full label from throttle list.\n", name);
        return NULL;
    }
    if (!match)
        fprintf(stderr, "Error: unknown throttle name '%s'. See throttle list.\n", name);
    return match;
}

static bool throttle_available(const alltrax_info* info, const throttle_entry* entry)
{
    return !(entry->xct_only && strncmp(info->model, "SRX", 3) == 0) &&
           (info->throttles_allowed & (1u << (entry->type - 1))) != 0;
}

static const char* linearization_preset(uint8_t type)
{
    switch (type) {
    case 1: return "0-5k-2wire";
    case 2: return "5k-0-2wire";
    case 5: return "yamaha";
    case 7: return "clubcar";
    default: return "linear";
    }
}

static bool hpd_forced_off(uint8_t type)
{
    return type >= 8 && type <= 10;  /* Digital, Pump and USB */
}

/* Read the selected throttle type and name from the settings page the library
 * writes: the user page, or the factory page while the user page is invalid. */
static alltrax_error read_current(alltrax_controller* ctrl, bool skip_goodset,
    alltrax_var_value out[2])
{
    const alltrax_var_def* vars[] = {
        alltrax_find_var("Throttle_Type"), alltrax_find_var("Throttle_Type_Name"),
    };
    if (!skip_goodset) {
        const alltrax_var_def* marker = alltrax_find_var("Var_GoodSet");
        alltrax_var_value value;
        alltrax_error err = alltrax_read_vars(ctrl, &marker, 1, &value);
        if (err) return err;
        if (value.raw.u16 != 0)
            return alltrax_read_factory_vars(ctrl, vars, 2, out);
    }
    return alltrax_read_vars(ctrl, vars, 2, out);
}

/* The current entry is the first available one with the stored type whose
 * label starts with the stored name, else the first with the stored type. */
static const throttle_entry* current_entry(const alltrax_info* info,
    uint8_t type, const char* name)
{
    const throttle_entry* first = NULL;
    size_t len = strlen(name);
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        const throttle_entry* entry = &catalog[i];
        if (entry->type != type || !throttle_available(info, entry)) continue;
        if (len && strncmp(entry->label, name, len) == 0) return entry;
        if (!first) first = entry;
    }
    return first;
}

static int set_throttle(alltrax_controller* ctrl, const throttle_entry* entry,
    uint8_t current, const cli_flags* flags)
{
    if (flags->default_curve && entry->type == 11 && current == 11) {
        fprintf(stderr, "Error: Absolute is already selected and keeps its "
                        "linearization; --default-curve is not available\n");
        return 1;
    }
    const alltrax_var_def* type = alltrax_find_var("Throttle_Type");
    const alltrax_var_def* name = alltrax_find_var("Throttle_Type_Name");
    char label[17];
    snprintf(label, sizeof(label), "%.*s", (int)name->size, entry->label);
    bool hpd = !hpd_forced_off(entry->type) && !flags->no_hpd;
    alltrax_assignment items[4] = {
        { .var = type, .number = entry->type },
        { .var = name, .text = label },
        { .var = alltrax_find_var("HPD"), .number = hpd },
    };
    size_t count = 3;

    alltrax_curve_data curve;
    if (flags->default_curve || current != entry->type) {
        const alltrax_curve_preset* preset =
            alltrax_find_curve_preset("linearization", linearization_preset(entry->type));
        if (!preset) {
            fprintf(stderr, "Error: no default linearization for %s\n", entry->label);
            return 1;
        }
        curve.def = alltrax_find_curve("linearization");
        memcpy(curve.x, preset->x, sizeof(curve.x));
        memcpy(curve.y, preset->y, sizeof(curve.y));
        items[count++] = (alltrax_assignment){ .curve = curve.def, .curve_data = &curve };
    }

    alltrax_write_report report;
    alltrax_write_opts opts = cli_write_opts(flags, &report);
    alltrax_error err = alltrax_write_assignments(ctrl, items, count, &opts);
    if (err)
        cli_error(ctrl, err, "setting throttle");
    else
        printf("Set throttle to %s (%u), HPD %s\n", entry->label,
               (unsigned)entry->type, hpd ? "on" : "off");
    cli_report_write(ctrl, &report, flags);
    return err != ALLTRAX_OK;
}

int cmd_throttle(int argc, char** argv)
{
    int rc = 1;
    bool set = argc >= 2 && strcmp(argv[1], "set") == 0;
    if (argc < 2 || (!set && strcmp(argv[1], "list") != 0)) {
        fprintf(stderr, "Usage: alltrax throttle list | throttle set [options] <name>\n");
        return 1;
    }

    cli_flags flags;
    const unsigned allowed = set ? CLI_OPTS_WRITE | CLI_OPT_DEFAULT_CURVE | CLI_OPT_NO_HPD : 0;
    const int positional = set ? 1 : 0;
    if (cli_parse_args(argc - 1, argv + 1, set ? "throttle set" : "throttle list",
                       allowed, positional, positional, &flags) < 0) {
        if (set) cli_print_options(stderr, allowed);
        return 1;
    }
    const throttle_entry* entry = set ? find_throttle(argv[2]) : NULL;
    if (set && !entry) return 1;
    if (set && flags.no_hpd && hpd_forced_off(entry->type)) {
        fprintf(stderr, "Error: HPD is always off for %s; --no-hpd does not apply\n",
                entry->label);
        return 1;
    }

    alltrax_controller* ctrl = NULL;
    if (cli_open(&ctrl, set)) return 1;

    alltrax_info info;
    alltrax_error err = alltrax_get_info(ctrl, &info);
    if (err) {
        cli_error(ctrl, err, "reading throttle capabilities");
    } else if (info.bootloader_mode) {
        fprintf(stderr, "Error: throttle is unavailable in bootloader mode\n");
    } else if (set && !throttle_available(&info, entry)) {
        fprintf(stderr, "Error: %s is not available on %s. See throttle list.\n",
                entry->label, info.model);
    } else {
        alltrax_var_value current[2];
        err = read_current(ctrl, set && flags.debug_no_goodset, current);
        if (err) {
            cli_error(ctrl, err, "reading current throttle");
        } else if (set) {
            rc = set_throttle(ctrl, entry, current[0].raw.u8, &flags);
        } else {
            const throttle_entry* selected =
                current_entry(&info, current[0].raw.u8, current[1].raw.str);
            for (size_t i = 0; i < CATALOG_COUNT; i++)
                if (throttle_available(&info, &catalog[i]))
                    printf("%c %2u  %s\n", &catalog[i] == selected ? '*' : ' ',
                           (unsigned)catalog[i].type, catalog[i].label);
            rc = 0;
        }
    }

    alltrax_close(ctrl);
    return rc;
}
