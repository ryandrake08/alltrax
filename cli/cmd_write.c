/*
 * cmd_write.c — alltrax write var=value [...]: Write settings
 *
 * Parses var=value pairs and writes them in one alltrax_write_assignments()
 * batch: one CAL/RUN bracket and one FLASH page cycle. String variables take
 * the text after '=' literally; an empty value clears the field.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include "cli.h"

int cmd_write(int argc, char** argv)
{
    cli_flags flags;
    const cli_option_mask allowed = CLI_OPTS_WRITE | CLI_OPT_SKIP_RULES;
    int n_assignments = cli_parse_args(argc, argv, "write", allowed, 1, -1,
                                       &flags);

    if (n_assignments < 0) {
        fprintf(stderr,
            "Usage: alltrax write [options] var=value ...\n"
            "\n"
            "String variables take the text after '=' (printable ASCII, up to\n"
            "the field size); an empty value clears the field.\n"
            "Profile currents accept a percentage or a magnitude with an A suffix.\n");
        cli_print_options(stderr, allowed);
        return 1;
    }

    /* One spare item for the field-map name. */
    alltrax_assignment* assigns = calloc((size_t)n_assignments + 1,
                                         sizeof(alltrax_assignment));
    bool* in_amps = calloc((size_t)n_assignments, sizeof(*in_amps));
    alltrax_controller* ctrl = NULL;
    int ret = 1;
    if (!assigns || !in_amps) {
        fprintf(stderr, "Error: out of memory\n");
        goto done;
    }

    /* Parse all var=value pairs */
    for (int i = 0; i < n_assignments; i++) {
        char* arg = argv[1 + i];
        char* eq = strchr(arg, '=');
        if (!eq || eq == arg) {
            fprintf(stderr, "Error: expected var=value, got: %s\n", arg);
            goto done;
        }

        *eq = '\0';
        const char* name = arg;
        const char* val_str = eq + 1;

        const alltrax_var_def* var = alltrax_find_var(name);
        if (!var) {
            fprintf(stderr, "Unknown variable: %s\n", name);
            goto done;
        }
        if (var->type == ALLTRAX_TYPE_STRING) {
            if (alltrax_validate_var_string(var, val_str)) {
                fprintf(stderr, "Error: invalid text for %s: printable ASCII, "
                        "at most %u characters\n", name, (unsigned)var->size);
                goto done;
            }
            assigns[i] = (alltrax_assignment){ .var = var, .text = val_str };
            continue;
        }

        /* Parse value — bools accept Yes/No/1/0/true/false */
        double value;
        if (var->type == ALLTRAX_TYPE_BOOL) {
            if (strcmp(val_str, "1") == 0 || strcmp(val_str, "true") == 0 ||
                strcmp(val_str, "Yes") == 0 || strcmp(val_str, "yes") == 0) {
                value = 1.0;
            } else if (strcmp(val_str, "0") == 0 || strcmp(val_str, "false") == 0 ||
                       strcmp(val_str, "No") == 0 || strcmp(val_str, "no") == 0) {
                value = 0.0;
            } else {
                fprintf(stderr, "Invalid boolean value for %s: %s "
                                "(use Yes/No/1/0/true/false)\n", name, val_str);
                goto done;
            }
        } else {
            char* endptr;
            errno = 0;
            value = strtod(val_str, &endptr);
            bool profile_current = cli_is_profile_current(var);
            bool amp_suffix = strcmp(endptr, "A") == 0 &&
                              (profile_current || strcmp(var->unit, "A") == 0);
            if (endptr == val_str || (*endptr != '\0' && !amp_suffix) ||
                errno == ERANGE || !isfinite(value) ||
                (amp_suffix && profile_current && value < 0)) {
                fprintf(stderr, "Invalid number for %s: %s\n", name, val_str);
                goto done;
            }
            in_amps[i] = amp_suffix && profile_current;
        }

        /* Profile amps are converted after reading the device rating. */
        alltrax_error verr = in_amps[i] ? ALLTRAX_OK : alltrax_validate_var_value(var, value);
        if (verr) {
            double bmin, bmax;
            alltrax_var_display_bounds(var, &bmin, &bmax);
            const char* u = var->unit;
            if (u[0])
                fprintf(stderr, "Error: %s value %g out of range [%g, %g] %s\n",
                        name, value, bmin, bmax, u);
            else
                fprintf(stderr, "Error: %s value %g out of range [%g, %g]\n",
                        name, value, bmin, bmax);
            goto done;
        }

        assigns[i] = (alltrax_assignment){ .var = var, .number = value };
    }

    size_t count = (size_t)n_assignments;
    cli_warn_field_map(assigns, count);

    alltrax_error err = cli_open(&ctrl, true);
    if (err) goto done;
    alltrax_info info;
    bool have_info;
    if (cli_convert_profile_currents(ctrl, assigns, in_amps, count, &info, &have_info))
        goto done;
    if (cli_rules_check(ctrl, assigns, count, have_info ? &info : NULL, &flags))
        goto done;

    alltrax_write_report report;
    alltrax_write_opts write_opts = cli_write_opts(&flags, &report);
    char field_map_name[33];

    err = cli_add_field_map_name(ctrl, assigns, &count, field_map_name);
    if (err) goto done;
    err = alltrax_write_assignments(ctrl, assigns, count, &write_opts);
    if (err) {
        cli_error(ctrl, err, "writing variables");
        cli_report_write(ctrl, &report);
        goto done;
    }

    for (size_t i = 0; i < count; i++)
        printf("Wrote %s (%s)\n", assigns[i].var->name,
               assigns[i].var->is_flash ? "FLASH" : "RAM");
    cli_field_map_written(assigns, count);

    cli_report_write(ctrl, &report);
    ret = 0;

done:
    free(in_amps);
    free(assigns);
    if (ctrl) alltrax_close(ctrl);
    return ret;
}
