/*
 * cmd_write.c — alltrax write var=value [...]: Write settings
 *
 * Parses var=value pairs, separates RAM and FLASH variables, and
 * writes them using the appropriate method. FLASH variables are
 * batched into a single page write cycle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"

/* A parsed assignment */
typedef struct {
    const alltrax_var_def* var;
    double value;
} assignment;

int cmd_write(int argc, char** argv)
{
    cli_flags flags;
    int first_arg = cli_parse_flags(argc, argv, &flags);

    if (first_arg >= argc) {
        fprintf(stderr,
            "Usage: alltrax write [flags] var=value ...\n"
            "\n"
            "Flags:\n"
            "  --no-cal           Skip CAL/RUN mode bracket\n"
            "  --no-verify        Skip read-back verification (FLASH)\n"
            "  --no-goodset       Skip GoodSet pre-check (allows write when\n"
            "                     a previous interrupted write left GoodSet invalid)\n"
            "  --no-fw-version    Skip firmware version check\n"
            "  --no-voltage-link  Skip voltage link validation\n"
            "                     (KSI < UnderVolt < OverVolt, min 1V gaps)\n"
            "  --reset            Reboot controller after FLASH write\n");
        return 1;
    }

    int n_assignments = argc - first_arg;
    assignment* assigns = calloc((size_t)n_assignments, sizeof(assignment));
    if (!assigns) {
        fprintf(stderr, "Error: out of memory\n");
        return 1;
    }

    /* Parse all var=value pairs */
    for (int i = 0; i < n_assignments; i++) {
        char* arg = argv[first_arg + i];
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
        if (!var->writable) {
            fprintf(stderr, "Variable is read-only: %s\n", name);
            free(assigns);
            return 1;
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
            value = strtod(val_str, &endptr);
            if (*endptr != '\0') {
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

        assigns[i].var = var;
        assigns[i].value = value;
    }

    /* Open with write access */
    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, true);
    if (err) {
        free(assigns);
        return 1;
    }

    int ret = 0;

    /* Build var/value arrays for the library */
    const alltrax_var_def** var_ptrs = calloc((size_t)n_assignments,
                                              sizeof(alltrax_var_def*));
    double* var_values = calloc((size_t)n_assignments, sizeof(double));
    if (!var_ptrs || !var_values) {
        fprintf(stderr, "Error: out of memory\n");
        ret = 1;
        goto done;
    }

    for (int i = 0; i < n_assignments; i++) {
        var_ptrs[i] = assigns[i].var;
        var_values[i] = assigns[i].value;
    }

    alltrax_write_opts write_opts = {
        .skip_cal = flags.no_cal,
        .skip_verify = flags.no_verify,
        .skip_goodset = flags.no_goodset,
        .skip_fw_check = flags.no_fw_version,
        .skip_voltage_link = flags.no_voltage_link,
    };

    err = alltrax_write_vars(ctrl, var_ptrs, var_values,
                             (size_t)n_assignments, &write_opts);
    if (err) {
        cli_error(ctrl, err, "writing variables");
        ret = 1;
        goto done;
    }

    for (int i = 0; i < n_assignments; i++)
        printf("Wrote %s (%s)\n", assigns[i].var->name,
               assigns[i].var->is_flash ? "FLASH" : "RAM");

    if (flags.reset) {
        err = alltrax_reset_device(ctrl);
        if (err) {
            cli_error(ctrl, err, "resetting device");
            ret = 1;
            goto done;
        }
        printf("Device reset sent (controller will reboot)\n");
    }

done:
    free(var_ptrs);
    free(var_values);
    free(assigns);
    alltrax_close(ctrl);
    return ret;
}
