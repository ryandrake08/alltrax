/* Read-only monitor and throttle view. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cli.h"

#define MONITOR_FAST_MS 100

static volatile sig_atomic_t running;

typedef struct {
    alltrax_info info;
    uint8_t throttle_type;
    int16_t user_state;
    bool speed_limit, bms;
    char field_map[33];
} monitor_config;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

static alltrax_error read_config(alltrax_controller* ctrl, monitor_config* config)
{
    alltrax_error err = alltrax_get_info(ctrl, &config->info);
    if (err) return err;
    const alltrax_var_def* user_marker = alltrax_find_var("Var_GoodSet");
    alltrax_var_value marker;
    err = alltrax_read_vars(ctrl, &user_marker, 1, &marker);
    if (err) return err;
    bool factory = marker.raw.u16 != 0;
    if (factory) {
        const alltrax_var_def* factory_marker = alltrax_find_var("Fact_GoodSet");
        err = alltrax_read_vars(ctrl, &factory_marker, 1, &marker);
        if (err) return err;
        if (marker.raw.u16 != 0) {
            fprintf(stderr, "Warning: both settings pages are invalid. Settings-dependent readings may be unavailable.\n");
            return ALLTRAX_OK;
        }
    }

    const alltrax_var_def* vars[] = {
        alltrax_find_var("Throttle_Type"), alltrax_find_var("BMS_Expected"),
        alltrax_find_var("Speed_Limit_On"), alltrax_find_var("UserInputs_State"),
        alltrax_find_var("F_Table_Name"),
    };
    alltrax_var_value values[sizeof(vars) / sizeof(vars[0])];
    size_t count = sizeof(vars) / sizeof(vars[0]);
    err = factory ? alltrax_read_factory_vars(ctrl, vars, count, values)
                  : alltrax_read_vars(ctrl, vars, count, values);
    if (err) return err;
    config->throttle_type = values[0].raw.u8;
    config->bms = config->info.has_bms_can && values[1].raw.b;
    config->speed_limit = values[2].raw.b;
    if (alltrax_has_feature(&config->info, ALLTRAX_FEAT_USER_PROFILES))
        config->user_state = values[3].raw.i16;
    snprintf(config->field_map, sizeof(config->field_map), "%s", values[4].raw.str);
    return ALLTRAX_OK;
}

static int percent(int raw)
{
    if (raw < 0) raw = 0;
    if (raw > 4095) raw = 4095;
    return raw * 100 / 4095;
}

static int throttle_percent(const monitor_config* c, const alltrax_monitor_data* d)
{
    if (!d->keyswitch) return 0;
    return percent(c->throttle_type == 9 || c->throttle_type == 10
                   ? d->throttle_pointer : d->throttle_position);
}

static double motor_volts(const alltrax_monitor_data* d)
{
    return d->battery_volts * d->throttle_local / 4095.0;
}

static int speed_rpm(const alltrax_monitor_data* d)
{
    return d->speed_rpm == 50 ? 0 : d->speed_rpm;
}

static bool soc_available(const alltrax_monitor_data* d)
{
    return d->keyswitch && !d->errors[14];
}

static bool error_enabled(const alltrax_monitor_data* d, int i)
{
    return d->keyswitch || i == 15 || i == 16; /* Aux Stuck and Bad Vars */
}

static bool error_active(const alltrax_monitor_data* d, int i)
{
    return error_enabled(d, i) && (d->errors[i] || (i == 6 && d->overtemp_cap < 4095));
}

static const char* profile_name(int profile)
{
    return profile == 1 ? "User 1" : profile == 2 ? "User 2" : "Normal";
}

static const char* on_off(bool value)
{
    return value ? "ON" : "off";
}

static void print_analog(const char* label, int value, bool ksi)
{
    printf("  %-21s ", label);
    if (!ksi) puts("-");
    else if (value < 0) puts("Disconnected");
    else printf("%d %%\n", value);
}

static void print_monitor(const monitor_config* c, const alltrax_monitor_data* d,
    int rate, bool interactive)
{
    if (interactive) printf("\033[2J\033[H");
    printf("Alltrax Controller Monitor (%s)\n", c->info.model);
    printf("  Battery Voltage       %.1f V\n", d->battery_volts);
    printf("  Battery Current       %.1f A\n", d->battery_amps);
    printf("  Motor Voltage         %.1f V\n", motor_volts(d));
    printf("  Motor Current         %.1f A\n", d->output_amps);
    printf("  Field Current         %.1f A\n", d->field_amps);
    if (d->keyswitch) printf("  Temperature           %.1f C\n", d->temp_c);
    else puts("  Temperature           -");
    if (c->info.speed_sensor && c->speed_limit)
        printf("  Motor Speed           %d RPM\n", speed_rpm(d));
    else
        puts("  Motor Speed           N/A");
    if (c->bms) {
        if (soc_available(d)) printf("  State of Charge       %.1f %%\n", d->state_of_charge);
        else puts("  State of Charge       -");
    }
    printf("  Throttle              %d %%\n", throttle_percent(c, d));
    printf("  Throttle Position     %d MU\n", d->throttle_position);
    printf("  Throttle Pointer      %d MU\n", d->throttle_pointer);
    printf("  Overtemp Cap          %d MU\n", d->overtemp_cap);

    printf("  Keyswitch             %s\n", on_off(d->keyswitch));
    printf("  Footswitch            %s\n", on_off(d->footswitch));
    if (c->info.has_forward) printf("  Forward               %s\n", on_off(d->forward));
    printf("  Reverse               %s\n", on_off(d->reverse));
    if (c->user_state == 1) {
        printf("  Selected Profile      %s\n", d->keyswitch ? profile_name(d->user_profile_num) : "-");
        print_analog("User1 (Regen)", d->user1_input_percent, d->keyswitch);
    } else if (c->user_state == 2) {
        print_analog("User2 (Speed)", d->user2_input_percent, d->keyswitch);
        print_analog("User1 (Regen)", d->user1_input_percent, d->keyswitch);
    } else {
        if (c->info.has_user1) printf("  User 1 Input          %s\n", on_off(d->user1_input));
        if (c->info.has_user2) printf("  User 2 Input          %s\n", on_off(d->user2_input));
    }
    printf("  User 3 Input          %s\n", d->keyswitch ? on_off(d->user3_input) : "-");
    printf("  Charger               %s\n", d->keyswitch ? on_off(d->charger) : "-");
    printf("  Main Relay            %s\n", on_off(d->relay_on > 0));
    printf("  Fan                   %s\n", on_off(d->fan_status));
    printf("  AuxOut                %s\n", on_off(d->check_motor));
    printf("  Blower                %s\n", on_off(d->blower));
    printf("  Horn                  %s\n", on_off(d->horn));
    printf("  HPD Ran               %s\n", on_off(d->hpd_ran));
    if (d->bad_vars_code != 0)
        printf("  Bad Vars Code         %u\n", (unsigned)d->bad_vars_code);

    bool any_error = false;
    for (int i = 0; i < 17; i++) {
        if (!error_enabled(d, i)) continue;
        if ((i == 14 || i == 15) && !c->bms) continue;
        bool active = error_active(d, i);
        if (active || d->error_history[i]) {
            printf("  %s: %s\n", active ? "Active" : "Previously occurred",
                   alltrax_error_flag_name(i));
            any_error = true;
        }
    }
    if (!d->keyswitch) puts("  Other errors unavailable with keyswitch off");
    else if (!any_error) puts("  No errors");
    printf("[Ctrl+C to stop]  Target interval %d ms\n\n", rate);
}

/* CSV uses the Toolkit's layout, conditional fields and separator columns.
 * Header and sample rows share the column order and inclusion rules. */
static void csv_value(FILE* file, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
}

static void csv_line(FILE* file, const monitor_config* c,
    const alltrax_monitor_data* d, const struct timespec* time)
{
    bool header = d == NULL;
    char date[16] = "", hours[16] = "", stamp[32] = "";
    if (!header) {
        struct tm tm;
        if (localtime_r(&time->tv_sec, &tm)) {
            strftime(date, sizeof(date), "%Y-%m-%d", &tm);
            strftime(hours, sizeof(hours), "%H:%M:%S", &tm);
        }
        if (gmtime_r(&time->tv_sec, &tm))
            strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
        fprintf(file, "%s,%s", date, hours);
    } else {
        fputs("Date,Time", file);
    }
#define COL(label, ...) do { \
    fputc(',', file); \
    if (header) fputs(label, file); \
    else csv_value(file, __VA_ARGS__); \
} while (0)
    COL("Keyswitch", "%d", d->keyswitch);
    COL("Footswitch", "%d", d->footswitch);
    COL("Forward Switch", "%d", d->forward);
    COL("Reverse Switch", "%d", d->reverse);
    if (c->user_state == 1) {
        COL("User Selected Profile", "%s", profile_name(d->user_profile_num));
        COL("User1 Input Percent (Regen)", "%d", d->user1_input_percent);
    } else if (c->user_state == 2) {
        COL("User2 Input Percent (Speed)", "%d", d->user2_input_percent);
        COL("User1 Input Percent (Regen)", "%d", d->user1_input_percent);
    } else {
        if (c->info.has_user1) COL("User 1 Input", "%d", d->user1_input);
        if (c->info.has_user2) COL("User 2 Input", "%d", d->user2_input);
    }
    COL("User 3 Input", "%d", d->keyswitch && d->user3_input);
    COL("Throttle %", "%d", throttle_percent(c, d));
    if (c->info.speed_sensor)
        COL(c->speed_limit ? "Speed" : "Estimated Speed", "%d", speed_rpm(d));
    if (c->bms)
        COL("State Of Charge", soc_available(d) ? "%.1f" : "", d->state_of_charge);
    COL("Battery Voltage", "%.1f", d->battery_volts);
    COL("Battery Current", "%.1f", d->battery_amps);
    COL("Motor Voltage", "%.1f", motor_volts(d));
    COL("Motor Current", "%.1f", d->output_amps);
    COL("Field Amps", "%.1f", d->field_amps);
    COL("Temperature", "%.1f", d->temp_c);
    COL("Fan Status", "%d", d->fan_status);
    COL("AuxOut Status", "%d", d->check_motor);
    COL("Blower Status", "%d", d->blower);
    COL("Horn Status", "%d", d->horn);
    COL("Relay Status", "%d", d->relay_on > 0);
    COL("HPD Ran", "%d", d->hpd_ran);
    COL("Charger Plugged In", "%d", d->keyswitch && d->charger);
    fputs(",,", file);
    static const char* const error_columns[17] = {
        "Global_Shutdown", "Hardware_Overcurrent", NULL, "LOBAT", "HIBAT",
        "Precharge_Fail", "Overtemp", "Undertemp", "Range_Alarm",
        "Hi_HThrot_Overange", "Lo_HThrot_Overange", "Hi_LThrot_Overange",
        "Lo_LThrot_Overange", "Field_Open_Alarm", "BMS_Missing", "AuxStuck", "Bad Vars",
    };
    for (int i = 0; i < 17; i++)
        if (error_columns[i] && (c->bms || (i != 14 && i != 15)))
            COL(error_columns[i], "%d", error_active(d, i));
    COL("Bad Vars Code", "%u", (unsigned)d->bad_vars_code);
    fputs(",,", file);
    COL("Throttle Position", "%.1f", (double)d->throttle_position);
    COL("Throttle Pointer", "%.1f", (double)d->throttle_pointer);
    COL("Overtemp Cap", "%d", d->overtemp_cap);
    COL("Timestamp", stamp[0] ? "%s.%03ldZ" : "", stamp, time->tv_nsec / 1000000L);
#undef COL
    fputc('\n', file);
}

static void csv_identity_field(FILE* file, const char* label, const char* value)
{
    bool quote = strpbrk(value, ",\"\r\n") != NULL;
    if (quote) fputc('"', file);
    fputs(label, file);
    for (; *value; value++) {
        if (*value == '"') fputc('"', file);
        fputc(*value, file);
    }
    if (quote) fputc('"', file);
}

static void write_log_header(FILE* file, const monitor_config* c)
{
    if (fseek(file, 0, SEEK_END) == 0 && ftell(file) > 0) fputs("\n\n", file);
    fputs("---------- ---------- ----------\n", file);
    csv_identity_field(file, "Model: ", c->info.model);
    fprintf(file, ",,,Serial: %" PRIu32 ",,,", c->info.serial_number);
    csv_identity_field(file, "Field Map: ", c->field_map);
    fputs("\n---------- ---------- ----------\n\n", file);
    csv_line(file, c, NULL, NULL);
}

static alltrax_error print_throttle(alltrax_controller* ctrl, bool interactive)
{
    const alltrax_var_def* vars[] = {
        alltrax_find_var("Keyswitch_Input"), alltrax_find_var("Throttle_Non_Linearized"),
        alltrax_find_var("Throttle_Linearized"), alltrax_find_var("Throttle_Pointer"),
        alltrax_find_var("Throttle_Local"),
    };
    alltrax_var_value values[sizeof(vars) / sizeof(vars[0])];
    alltrax_error err = alltrax_read_vars(ctrl, vars, sizeof(vars) / sizeof(vars[0]), values);
    if (err || !running) return err;
    if (interactive) printf("\033[2J\033[H");
    if (values[0].raw.b) {
        printf("Keyswitch ON   Input %d %%   Linearized %d %%   Setpoint %d %%   Output %d %%\n",
               percent(values[1].raw.i16), percent(values[2].raw.i16),
               percent(values[3].raw.i16), percent(values[4].raw.i16));
    } else {
        puts("Keyswitch off  Input       Linearized       Setpoint       Output");
    }
    return ALLTRAX_OK;
}

static void wait_for_tick(const struct timespec* start, int rate)
{
    struct timespec now;
    if (!running || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
    int64_t elapsed = (int64_t)(now.tv_sec - start->tv_sec) * 1000000000 +
                      now.tv_nsec - start->tv_nsec;
    int64_t remaining = (int64_t)rate * 1000000 - elapsed;
    if (remaining > 0)
        nanosleep(&(struct timespec){ (time_t)(remaining / 1000000000),
                                      (long)(remaining % 1000000000) }, NULL);
}

int cmd_monitor(int argc, char** argv)
{
    cli_flags flags;
    const cli_option_mask allowed = CLI_OPT_MONITOR_RATE | CLI_OPT_MONITOR_LOG;
    int nargs = cli_parse_args(argc, argv, "monitor", allowed, 0, 1, &flags);
    bool throttle_view = nargs == 1 && strcmp(argv[1], "throttle") == 0;
    if (nargs < 0 || (nargs == 1 && !throttle_view)) {
        if (nargs == 1)
            fprintf(stderr, "Error: monitor: unknown mode %s\n", argv[1]);
        fprintf(stderr, "Usage: alltrax monitor [--rate <ms>] [--log <file.csv>] | monitor throttle\n");
        cli_print_options(stderr, allowed);
        return 1;
    }
    if (throttle_view && (flags.monitor_rate || flags.monitor_log)) {
        fprintf(stderr, "Error: monitor throttle uses a fixed interval and does not accept --rate or --log\n");
        return 1;
    }
    int rate = MONITOR_FAST_MS;
    if (flags.monitor_rate && strcmp(flags.monitor_rate, "100") != 0) {
        if (strcmp(flags.monitor_rate, "1000") == 0) rate = 1000;
        else if (strcmp(flags.monitor_rate, "3000") == 0) rate = 3000;
        else {
            fprintf(stderr, "Error: --rate must be 100, 1000, or 3000 ms\n");
            return 1;
        }
    }
    if (flags.monitor_log && !flags.monitor_log[0]) {
        fprintf(stderr, "Error: --log requires a filename\n");
        return 1;
    }

    int rc = 1;
    FILE* log = NULL;
    alltrax_controller* ctrl = NULL;
    if (flags.monitor_log) {
        log = fopen(flags.monitor_log, "a");
        if (!log) {
            fprintf(stderr, "Error: could not open monitor log '%s': %s\n",
                    flags.monitor_log, strerror(errno));
            return 1;
        }
    }
    if (cli_open(&ctrl, false)) goto done;
    monitor_config config = {0};
    alltrax_error err;
    if (!throttle_view && (err = read_config(ctrl, &config))) {
        cli_error(ctrl, err, "reading monitor settings");
        goto done;
    }
    if (log) {
        write_log_header(log, &config);
        if (ferror(log) || fflush(log) != 0) {
            fprintf(stderr, "Error: could not write header to monitor log '%s'\n", flags.monitor_log);
            goto done;
        }
    }

    struct sigaction sa = { .sa_handler = sigint_handler }, old;
    sigemptyset(&sa.sa_mask);
    running = 1;
    if (sigaction(SIGINT, &sa, &old) != 0) {
        perror("Error: cannot handle Ctrl+C");
        goto done;
    }

    bool interactive = isatty(fileno(stdout));
    while (running) {
        struct timespec start;
        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
            perror("Error: reading polling clock");
            break;
        }
        if (throttle_view) {
            err = print_throttle(ctrl, interactive);
        } else {
            alltrax_monitor_data data;
            err = alltrax_read_monitor(ctrl, &data);
            if (!err && running) {
                if (log) {
                    struct timespec now;
                    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
                        perror("Error: reading sample timestamp");
                        break;
                    }
                    csv_line(log, &config, &data, &now);
                    if (ferror(log) || fflush(log) != 0) {
                        fprintf(stderr, "Error: could not append to monitor log '%s'\n", flags.monitor_log);
                        break;
                    }
                }
                print_monitor(&config, &data, rate, interactive);
            }
        }
        if (!running) break;
        if (err) {
            cli_error(ctrl, err, "reading monitor data");
            break;
        }
        fflush(stdout);
        wait_for_tick(&start, rate);
    }

    if (!running) {
        printf("Stopped.\n");
        rc = 0;
    }
    sigaction(SIGINT, &old, NULL);

done:
    if (log && fclose(log) != 0) {
        fprintf(stderr, "Error: closing monitor log '%s'\n", flags.monitor_log);
        rc = 1;
    }
    if (ctrl) alltrax_close(ctrl);
    return rc;
}
