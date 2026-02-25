/*
 * cmd_curve.c — alltrax curve: Read/write curve tables
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void print_curve_table(const alltrax_curve_data* curve)
{
    const alltrax_curve_def* def = curve->def;
    printf("%-14s  %16s (%s)  %16s (%s)\n",
           def->name, "X", def->x_unit, "Y", def->y_unit);
    printf("%-14s  %16s       %16s\n", "", "--------", "--------");
    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
        printf("  Point %-6d  %16.2f       %16.2f\n",
               i, curve->x[i], curve->y[i]);
    }
}

static void print_curve_csv(const alltrax_curve_data* curve, FILE* out)
{
    const alltrax_curve_def* def = curve->def;
    fprintf(out, "# %s curve\n", def->description);
    fprintf(out, "# X (%s), Y (%s)\n", def->x_unit, def->y_unit);
    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++)
        fprintf(out, "%.4f, %.4f\n", curve->x[i], curve->y[i]);
}

static void print_ascii_plot(const alltrax_curve_data* curve)
{
    const alltrax_curve_def* def = curve->def;
    const int width = 60;
    const int height = 20;

    /* Find data range */
    double xmin = curve->x[0], xmax = curve->x[0];
    double ymin = curve->y[0], ymax = curve->y[0];
    for (int i = 1; i < ALLTRAX_CURVE_POINTS; i++) {
        if (curve->x[i] < xmin) xmin = curve->x[i];
        if (curve->x[i] > xmax) xmax = curve->x[i];
        if (curve->y[i] < ymin) ymin = curve->y[i];
        if (curve->y[i] > ymax) ymax = curve->y[i];
    }

    /* Avoid division by zero for flat curves */
    if (xmax == xmin) xmax = xmin + 1.0;
    if (ymax == ymin) ymax = ymin + 1.0;

    /* Build character grid */
    char grid[20][61];
    memset(grid, ' ', sizeof(grid));
    for (int r = 0; r < height; r++)
        grid[r][width] = '\0';

    /* Plot points */
    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
        int col = (int)((curve->x[i] - xmin) / (xmax - xmin) * (width - 1));
        int row = (int)((curve->y[i] - ymin) / (ymax - ymin) * (height - 1));
        row = (height - 1) - row;  /* flip Y axis */
        if (col >= 0 && col < width && row >= 0 && row < height)
            grid[row][col] = '*';
    }

    printf("\n  %s (%s vs %s)\n\n", def->description, def->y_unit,
           def->x_unit);

    for (int r = 0; r < height; r++) {
        double yval = ymax - (double)r / (height - 1) * (ymax - ymin);
        printf("  %8.1f |%s\n", yval, grid[r]);
    }
    printf("           +");
    for (int i = 0; i < width; i++) printf("-");
    printf("\n");
    printf("           %-8.1f%*s%8.1f\n", xmin, width - 16, "", xmax);
    printf("           %*s%s (%s)\n", (width - 4) / 2, "", "X", def->x_unit);
    printf("\n");
}

/* Parse CSV file into curve_data. Returns 0 on success. */
static int parse_csv_file(const char* filepath, alltrax_curve_data* curve)
{
    FILE* f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open file: %s\n", filepath);
        return 1;
    }

    char line[256];
    int count = 0;

    while (fgets(line, sizeof(line), f) && count < ALLTRAX_CURVE_POINTS) {
        /* Skip comments and blank lines */
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        char* endptr;
        double x = strtod(p, &endptr);
        if (endptr == p) {
            fprintf(stderr, "Error: bad X value on line: %s", line);
            fclose(f);
            return 1;
        }

        /* Skip comma/whitespace */
        p = endptr;
        while (*p == ' ' || *p == '\t' || *p == ',') p++;

        double y = strtod(p, &endptr);
        if (endptr == p) {
            fprintf(stderr, "Error: bad Y value on line: %s", line);
            fclose(f);
            return 1;
        }

        curve->x[count] = x;
        curve->y[count] = y;
        count++;
    }

    fclose(f);

    if (count == 0) {
        fprintf(stderr, "Error: no data points found in %s\n", filepath);
        return 1;
    }

    /* Pad remaining points with 0,0 */
    for (int i = count; i < ALLTRAX_CURVE_POINTS; i++) {
        curve->x[i] = 0.0;
        curve->y[i] = 0.0;
    }

    return 0;
}

/* Parse x,y pairs from argv. Returns number of points parsed, or -1 on error. */
static int parse_xy_args(int argc, char** argv, int start,
    alltrax_curve_data* curve)
{
    int count = 0;

    for (int i = start; i < argc && count < ALLTRAX_CURVE_POINTS; i++) {
        /* Skip flags */
        if (argv[i][0] == '-')
            continue;

        char* comma = strchr(argv[i], ',');
        if (!comma) {
            fprintf(stderr, "Error: expected x,y pair, got: %s\n", argv[i]);
            return -1;
        }

        char* endptr;
        double x = strtod(argv[i], &endptr);
        if (endptr != comma) {
            fprintf(stderr, "Error: bad X value in: %s\n", argv[i]);
            return -1;
        }

        double y = strtod(comma + 1, &endptr);
        if (*endptr != '\0') {
            fprintf(stderr, "Error: bad Y value in: %s\n", argv[i]);
            return -1;
        }

        curve->x[count] = x;
        curve->y[count] = y;
        count++;
    }

    /* Pad remaining points with 0,0 */
    for (int i = count; i < ALLTRAX_CURVE_POINTS; i++) {
        curve->x[i] = 0.0;
        curve->y[i] = 0.0;
    }

    return count;
}

/* ------------------------------------------------------------------ */
/* curve get <type> [--plot] [--file f.csv]                            */
/* ------------------------------------------------------------------ */

static int do_get(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: alltrax curve get <type> [--plot] [--file f.csv]\n"
            "\n"
            "Types: linearization, speed, torque, field, genfield, all\n");
        return 1;
    }

    /* Parse options */
    const char* type = NULL;
    const char* filepath = NULL;
    bool plot = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--plot") == 0) {
            plot = true;
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            filepath = argv[++i];
        } else if (argv[i][0] != '-') {
            if (!type) type = argv[i];
        }
    }

    if (!type) {
        fprintf(stderr, "Error: curve type required\n");
        return 1;
    }

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;

    FILE* csvfile = NULL;
    if (filepath) {
        csvfile = fopen(filepath, "w");
        if (!csvfile) {
            fprintf(stderr, "Error: cannot create file: %s\n", filepath);
            alltrax_close(ctrl);
            return 1;
        }
    }

    int ret = 0;
    bool all = strcmp(type, "all") == 0;

    size_t ncurves = alltrax_curve_count();
    for (size_t i = 0; i < ncurves; i++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(i);
        if (!all && strcmp(def->name, type) != 0)
            continue;

        alltrax_curve_data curve;
        err = alltrax_read_curve(ctrl, def, &curve);
        if (err) {
            cli_error(ctrl, err, def->name);
            ret = 1;
            break;
        }

        if (csvfile) {
            print_curve_csv(&curve, csvfile);
            if (all && i + 1 < ncurves)
                fprintf(csvfile, "\n");
        } else if (plot) {
            print_ascii_plot(&curve);
        } else {
            print_curve_table(&curve);
            if (all && i + 1 < ncurves)
                printf("\n");
        }

        if (!all) break;
    }

    if (!all && ret == 0) {
        /* Check if we found the curve */
        if (!alltrax_find_curve(type)) {
            fprintf(stderr, "Unknown curve type: %s\n", type);
            ret = 1;
        }
    }

    if (csvfile) {
        fclose(csvfile);
        if (ret == 0)
            printf("Wrote %s to %s\n", all ? "all curves" : type, filepath);
    }

    alltrax_close(ctrl);
    return ret;
}

/* ------------------------------------------------------------------ */
/* curve set <type> [--file f.csv] [x,y ...] [--no-cal] [--reset]      */
/* ------------------------------------------------------------------ */

static int do_set(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: alltrax curve set <type> [--file f.csv] [x,y ...]\n"
            "\n"
            "Types: linearization, speed, torque, field, genfield\n"
            "\n"
            "Flags:\n"
            "  --file <f.csv>   Read points from CSV file\n"
            "  --no-cal         Skip CAL/RUN mode bracket\n"
            "  --no-verify      Skip read-back verification\n"
            "  --no-fw-version  Skip firmware version check\n"
            "  --reset          Reboot controller after write\n");
        return 1;
    }

    cli_flags flags;
    int first_arg = cli_parse_flags(argc, argv, &flags);

    /* Find type and --file */
    const char* type = NULL;
    const char* filepath = NULL;
    int xy_start = -1;

    for (int i = first_arg; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            filepath = argv[++i];
        } else if (argv[i][0] != '-') {
            if (!type) {
                type = argv[i];
                xy_start = i + 1;
            }
        }
    }

    if (!type) {
        fprintf(stderr, "Error: curve type required\n");
        return 1;
    }

    const alltrax_curve_def* def = alltrax_find_curve(type);
    if (!def) {
        fprintf(stderr, "Unknown curve type: %s\n", type);
        return 1;
    }

    alltrax_curve_data curve;
    curve.def = def;

    if (filepath) {
        if (parse_csv_file(filepath, &curve))
            return 1;
    } else if (xy_start >= 0 && xy_start < argc) {
        int npoints = parse_xy_args(argc, argv, xy_start, &curve);
        if (npoints < 0)
            return 1;
        if (npoints == 0) {
            fprintf(stderr, "Error: no x,y points provided\n");
            return 1;
        }
    } else {
        fprintf(stderr, "Error: provide x,y pairs or --file\n");
        return 1;
    }

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, true);
    if (err) return 1;

    alltrax_write_opts opts = {
        .skip_cal = flags.no_cal,
        .skip_verify = flags.no_verify,
        .skip_fw_check = flags.no_fw_version,
    };

    err = alltrax_write_curve(ctrl, def, &curve, &opts);
    if (err) {
        cli_error(ctrl, err, "writing curve");
        alltrax_close(ctrl);
        return 1;
    }

    printf("Wrote %s curve (%d points)\n", def->name, ALLTRAX_CURVE_POINTS);

    if (flags.reset) {
        err = alltrax_reset_device(ctrl);
        if (err) {
            cli_error(ctrl, err, "resetting device");
            alltrax_close(ctrl);
            return 1;
        }
        printf("Device reset sent (controller will reboot)\n");
    }

    alltrax_close(ctrl);
    return 0;
}

/* ------------------------------------------------------------------ */
/* curve diff <type>                                                   */
/* ------------------------------------------------------------------ */

static int do_diff(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: alltrax curve diff <type>\n"
            "\n"
            "Types: linearization, speed, torque, field, genfield, all\n");
        return 1;
    }

    const char* type = argv[1];
    bool all = strcmp(type, "all") == 0;

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;

    int ret = 0;
    bool found = false;
    size_t ncurves = alltrax_curve_count();

    for (size_t i = 0; i < ncurves; i++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(i);
        if (!all && strcmp(def->name, type) != 0)
            continue;
        found = true;

        alltrax_curve_data user, factory;

        err = alltrax_read_curve(ctrl, def, &user);
        if (err) {
            cli_error(ctrl, err, def->name);
            ret = 1;
            break;
        }

        err = alltrax_read_curve_factory(ctrl, def, &factory);
        if (err) {
            cli_error(ctrl, err, def->name);
            ret = 1;
            break;
        }

        printf("%-14s  %8s (%s)  %8s (%s)  |  %8s (%s)  %8s (%s)\n",
               def->name,
               "User X", def->x_unit, "User Y", def->y_unit,
               "Fact X", def->x_unit, "Fact Y", def->y_unit);
        printf("%-14s  %8s       %8s       |  %8s       %8s\n",
               "", "------", "------", "------", "------");

        for (int p = 0; p < ALLTRAX_CURVE_POINTS; p++) {
            bool x_diff = user.x[p] != factory.x[p];
            bool y_diff = user.y[p] != factory.y[p];
            const char* marker = (x_diff || y_diff) ? " *" : "";

            printf("  Point %-6d  %8.2f       %8.2f       |  %8.2f"
                   "       %8.2f%s\n",
                   p, user.x[p], user.y[p],
                   factory.x[p], factory.y[p], marker);
        }

        if (all && i + 1 < ncurves)
            printf("\n");

        if (!all) break;
    }

    if (!found && !all) {
        fprintf(stderr, "Unknown curve type: %s\n", type);
        ret = 1;
    }

    alltrax_close(ctrl);
    return ret;
}

/* ------------------------------------------------------------------ */
/* curve reset <type> — copy factory curve to user                     */
/* ------------------------------------------------------------------ */

static int do_curve_reset(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: alltrax curve reset <type> [flags]\n"
            "\n"
            "Types: linearization, speed, torque, field, genfield, all\n"
            "\n"
            "Flags:\n"
            "  --no-cal         Skip CAL/RUN mode bracket\n"
            "  --no-verify      Skip read-back verification\n"
            "  --no-fw-version  Skip firmware version check\n"
            "  --reset          Reboot controller after write\n");
        return 1;
    }

    cli_flags flags;
    cli_parse_flags(argc, argv, &flags);

    /* Find the type arg (first non-flag) */
    const char* type = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            type = argv[i];
            break;
        }
    }

    if (!type) {
        fprintf(stderr, "Error: curve type required\n");
        return 1;
    }

    bool all = strcmp(type, "all") == 0;

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, true);
    if (err) return 1;

    alltrax_write_opts opts = {
        .skip_cal = flags.no_cal,
        .skip_verify = flags.no_verify,
        .skip_fw_check = flags.no_fw_version,
    };

    int ret = 0;
    bool found = false;
    size_t ncurves = alltrax_curve_count();

    for (size_t i = 0; i < ncurves; i++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(i);
        if (!all && strcmp(def->name, type) != 0)
            continue;
        found = true;

        alltrax_curve_data factory;
        err = alltrax_read_curve_factory(ctrl, def, &factory);
        if (err) {
            cli_error(ctrl, err, def->name);
            ret = 1;
            break;
        }

        err = alltrax_write_curve(ctrl, def, &factory, &opts);
        if (err) {
            cli_error(ctrl, err, def->name);
            ret = 1;
            break;
        }

        printf("Reset %s curve to factory defaults\n", def->name);
        if (!all) break;
    }

    if (!found && !all) {
        fprintf(stderr, "Unknown curve type: %s\n", type);
        ret = 1;
    }

    if (ret == 0 && flags.reset) {
        err = alltrax_reset_device(ctrl);
        if (err) {
            cli_error(ctrl, err, "resetting device");
            alltrax_close(ctrl);
            return 1;
        }
        printf("Device reset sent (controller will reboot)\n");
    }

    alltrax_close(ctrl);
    return ret;
}

/* ------------------------------------------------------------------ */
/* curve list                                                          */
/* ------------------------------------------------------------------ */

static int do_list(void)
{
    size_t ncurves = alltrax_curve_count();
    printf("%-16s  %-34s  %s\n", "Name", "Description", "Units (X / Y)");
    printf("%-16s  %-34s  %s\n", "----", "-----------", "-------------");
    for (size_t i = 0; i < ncurves; i++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(i);
        printf("%-16s  %-34s  %s / %s\n",
               def->name, def->description, def->x_unit, def->y_unit);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* alltrax curve <list|get|set|diff|reset>                             */
/* ------------------------------------------------------------------ */

int cmd_curve(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: alltrax curve <command> [options]\n"
            "\n"
            "Commands:\n"
            "  list          List available curve types\n"
            "  get <type>    Read curve from device\n"
            "  set <type>    Write curve to device\n"
            "  diff <type>   Compare user vs factory curve\n"
            "  reset <type>  Restore factory curve\n"
            "\n"
            "Types: linearization, speed, torque, field, genfield, all\n");
        return 1;
    }

    const char* subcmd = argv[1];

    if (strcmp(subcmd, "list") == 0)
        return do_list();
    if (strcmp(subcmd, "get") == 0)
        return do_get(argc - 1, argv + 1);
    if (strcmp(subcmd, "set") == 0)
        return do_set(argc - 1, argv + 1);
    if (strcmp(subcmd, "diff") == 0)
        return do_diff(argc - 1, argv + 1);
    if (strcmp(subcmd, "reset") == 0)
        return do_curve_reset(argc - 1, argv + 1);

    fprintf(stderr, "Unknown curve subcommand: %s\n", subcmd);
    return 1;
}
