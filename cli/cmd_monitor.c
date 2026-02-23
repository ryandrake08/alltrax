/*
 * cmd_monitor.c — alltrax monitor: Live polling (Ctrl+C to stop)
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <signal.h>
#include <time.h>
#include "cli.h"

static volatile sig_atomic_t running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void print_monitor(const alltrax_monitor_data* d)
{
    printf("\033[2J\033[H");  /* Clear screen, move to top-left */

    printf("============================================================\n");
    printf("Alltrax Controller Monitor\n");
    printf("============================================================\n");

    /* Errors */
    int any_error = 0;
    for (int i = 0; i < 17; i++) {
        if (d->errors[i]) {
            if (!any_error) {
                printf("\n  ERRORS ACTIVE:\n");
                any_error = 1;
            }
            printf("    !! %s (%d)\n", alltrax_error_flag_name(i), d->errors[i]);
        }
    }
    if (!any_error)
        printf("\n  No errors\n");

    /* Gauges */
    printf("\n--- Gauges ---\n");
    printf("  Battery Voltage     %7.1f V\n", d->battery_volts);
    printf("  Output Current      %7.1f A\n", d->output_amps);
    printf("  Battery Current     %7.1f A\n", d->battery_amps);
    printf("  Field Current       %7.2f A\n", d->field_amps);
    printf("  Temperature         %7.1f C\n", d->temp_c);
    printf("  State of Charge     %7.1f %%\n", d->state_of_charge);
    printf("  Motor Speed         %7d RPM\n", d->speed_rpm);

    /* Throttle */
    printf("\n--- Throttle ---\n");
    printf("  Local               %7d MU\n", d->throttle_local);
    printf("  Position            %7d MU\n", d->throttle_position);
    printf("  Setpoint (pointer)  %7d MU\n", d->throttle_pointer);

    /* Digital inputs */
    printf("\n--- Digital Inputs ---\n");
    printf("  Key Switch          %s\n", d->keyswitch  ? "ON" : "off");
    printf("  Forward             %s\n", d->forward    ? "ON" : "off");
    printf("  Reverse             %s\n", d->reverse    ? "ON" : "off");
    printf("  Foot Switch         %s\n", d->footswitch ? "ON" : "off");
    printf("  Main Relay          %s\n", d->relay_on   ? "ON" : "off");
    printf("  Fan                 %s\n", d->fan_status ? "ON" : "off");
    printf("  Charger             %s\n", d->charger    ? "ON" : "off");
    printf("  Blower              %s\n", d->blower     ? "ON" : "off");
    printf("  Horn                %s\n", d->horn       ? "ON" : "off");
    printf("  HPD Ran             %s\n", d->hpd_ran    ? "ON" : "off");

    printf("\n  [Ctrl+C to stop]  Polling every 250ms\n");
    fflush(stdout);
}

int cmd_monitor(int argc, char** argv)
{
    (void)argc; (void)argv;

    alltrax_controller* ctrl = NULL;
    alltrax_error err = cli_open(&ctrl, false);
    if (err) return 1;

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    while (running) {
        alltrax_monitor_data data;
        err = alltrax_read_monitor(ctrl, &data);
        if (err) {
            if (!running) break;  /* SIGINT during read */
            cli_error(ctrl, err, "reading monitor data");
            alltrax_close(ctrl);
            return 1;
        }
        print_monitor(&data);
        nanosleep(&(struct timespec){ 0, 250000000 }, NULL);
    }

    printf("\nStopped.\n");
    alltrax_close(ctrl);
    return 0;
}
