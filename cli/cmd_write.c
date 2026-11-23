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
#include "cli.h"

int cmd_write(int argc, char** argv)
{
    cli_flags flags;
    const unsigned allowed = CLI_OPTS_WRITE | CLI_OPT_NO_VOLTAGE_LINK |
                             CLI_OPT_FIELD_MAP;
    int n_assignments = cli_parse_args(argc, argv, "write", allowed, 1, -1,
                                       &flags);

    if (n_assignments < 0) {
        fprintf(stderr,
            "Usage: alltrax write [options] var=value ...\n"
            "\n"
            "String variables take the text after '=' (printable ASCII, up to\n"
            "the field size); an empty value clears the field.\n");
        cli_print_options(stderr, allowed);
        return 1;
    }

    /* One spare item for the field-map name. */
    alltrax_assignment* assigns = calloc((size_t)n_assignments + 1,
                                         sizeof(alltrax_assignment));
    if (!assigns) {
        fprintf(stderr, "Error: out of memory\n");
        return 1;
    }

    /* Parse all var=value pairs */
    for (int i = 0; i < n_assignments; i++) {
        char* arg = argv[1 + i];
        char* eq = strchr(arg, '=');
        if (!eq || eq == arg) {
            fprintf(stderr, "Error: expected var=value, got: %s\n", arg);
            free(assigns);
            return 1;
        }

        *eq = '\0';
        const char* name = arg;
        const char* val_str = eq + 1;

        const alltrax_var_def* var = alltrax_find_var(name);
        if (!var) {
            fprintf(stderr, "Unknown variable: %s\n", name);
            free(assigns);
            return 1;
        }
        if (var->type == ALLTRAX_TYPE_STRING) {
            if (alltrax_validate_var_string(var, val_str)) {
                fprintf(stderr, "Error: invalid text for %s: printable ASCII, "
                        "at most %u characters\n", name, (unsigned)var->size);
                free(assigns);
                return 1;
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
                free(assigns);
                return 1;
            }
        } else {
            char* endptr;
            errno = 0;
            value = strtod(val_str, &endptr);
            if (endptr == val_str || *endptr != '\0' || errno == ERANGE) {
                fprintf(stderr, "Invalid number for %s: %s\n", name, val_str);
                free(assigns);
                return 1;
            }
        }

        /* Validate value against bounds */
        alltrax_error verr = alltrax_validate_var_value(var, value);
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
            free(assigns);
            return 1;
        }

        assigns[i] = (alltrax_assignment){ .var = var, .number = value };
    }

    size_t count = (size_t)n_assignments;
    if (cli_check_field_map(assigns, count, flags.field_map)) {
        free(assigns);
        return 1;
    }

    /* Open with write access */
    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, true);
    if (err) {
        free(assigns);
        return 1;
    }

    int ret = 0;
    alltrax_write_report report;
    alltrax_write_opts write_opts = cli_write_opts(&flags, &report);
    char field_map_name[33];

    err = cli_add_field_map_name(ctrl, assigns, &count, field_map_name,
                                 flags.debug_no_goodset);
    if (err) {
        ret = 1;
        goto done;
    }
    err = alltrax_write_assignments(ctrl, assigns, count, &write_opts);
    if (err) {
        cli_error(ctrl, err, "writing variables");
        cli_print_write_report(&report);
        ret = 1;
        goto done;
    }

    for (size_t i = 0; i < count; i++)
        printf("Wrote %s (%s)\n", assigns[i].var->name,
               assigns[i].var->is_flash ? "FLASH" : "RAM");
    cli_field_map_written(assigns, count);

    cli_print_write_report(&report);

/* Success and write/reset failures release the batch and close the device. */
done:
    free(assigns);
    alltrax_close(ctrl);
    return ret;
}
