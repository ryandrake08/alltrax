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
        fprintf(stderr, "Usage: alltrax write [--no-cal] [--no-verify] "
                        "[--force] var=value ...\n");
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

    /* Separate RAM and FLASH variables into batches */
    const alltrax_var_def** ram_vars = NULL;
    double* ram_values = NULL;
    size_t ram_count = 0;
    const alltrax_var_def** flash_vars = NULL;
    double* flash_values = NULL;
    size_t flash_count = 0;
    int ret = 0;

    ram_vars = calloc((size_t)n_assignments, sizeof(alltrax_var_def*));
    ram_values = calloc((size_t)n_assignments, sizeof(double));
    flash_vars = calloc((size_t)n_assignments, sizeof(alltrax_var_def*));
    flash_values = calloc((size_t)n_assignments, sizeof(double));
    if (!ram_vars || !ram_values || !flash_vars || !flash_values) {
        fprintf(stderr, "Error: out of memory\n");
        ret = 1;
        goto done;
    }

    for (int i = 0; i < n_assignments; i++) {
        if (assigns[i].var->is_flash) {
            flash_vars[flash_count] = assigns[i].var;
            flash_values[flash_count] = assigns[i].value;
            flash_count++;
        } else {
            ram_vars[ram_count] = assigns[i].var;
            ram_values[ram_count] = assigns[i].value;
            ram_count++;
        }
    }

    /* Batch write RAM variables in one CAL/RUN bracket */
    if (ram_count > 0) {
        err = alltrax_write_ram_vars(ctrl, ram_vars, ram_values, ram_count);
        if (err) {
            cli_error(ctrl, err, "writing RAM variables");
            ret = 1;
            goto done;
        }
        for (size_t i = 0; i < ram_count; i++)
            printf("Wrote %s (RAM)\n", ram_vars[i]->name);
    }

    /* Batch write FLASH variables in one page cycle */
    if (flash_count > 0) {
        err = alltrax_write_flash_vars(ctrl, flash_vars, flash_values,
                                       flash_count);
        if (err) {
            cli_error(ctrl, err, "writing FLASH settings");
            ret = 1;
            goto done;
        }
        for (size_t i = 0; i < flash_count; i++)
            printf("Wrote %s (FLASH)\n", flash_vars[i]->name);

        /* Send device reset so firmware reloads from FLASH */
        err = alltrax_reset_device(ctrl);
        if (err) {
            cli_error(ctrl, err, "resetting device");
            ret = 1;
            goto done;
        }
        printf("Device reset sent (controller will reboot)\n");
    }

done:
    free(ram_vars);
    free(ram_values);
    free(flash_vars);
    free(flash_values);
    free(assigns);
    alltrax_close(ctrl);
    return ret;
}
