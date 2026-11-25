/*
 * cmd_debug_read.c — alltrax debug-read: hex dump of controller memory
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include "cli.h"

#define WATCH_MIN_MS 100
#define WATCH_MAX_MS 60000

static volatile sig_atomic_t running;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* Parse a decimal or 0x-prefixed hexadecimal number no larger than max. */
static bool parse_number(const char* text, unsigned long long max,
    unsigned long long* out)
{
    if (text[0] < '0' || text[0] > '9') return false;
    char* end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || *end || value > max) return false;
    *out = value;
    return true;
}

/* Sixteen bytes per line; a 2- or 4-byte read also shows its
 * little-endian value. */
static void print_dump(const char* prefix, uint32_t address,
    const uint8_t* buf, size_t length)
{
    for (size_t offset = 0; offset < length; offset += 16) {
        printf("%s0x%08X ", prefix, (unsigned)(address + offset));
        for (size_t i = offset; i < length && i < offset + 16; i++)
            printf(" %02x", buf[i]);
        if (length == 2)
            printf("  (u16 %u)", (unsigned)(buf[0] | buf[1] << 8));
        else if (length == 4)
            printf("  (u32 %lu)", (unsigned long)buf[0] |
                   (unsigned long)buf[1] << 8 | (unsigned long)buf[2] << 16 |
                   (unsigned long)buf[3] << 24);
        printf("\n");
    }
}

int cmd_debug_read(int argc, char** argv)
{
    cli_flags flags;
    int nargs = cli_parse_args(argc, argv, "debug-read", CLI_OPT_WATCH, 1, 2, &flags);
    unsigned long long address = 0, length = 4, watch_ms = 0;
    bool valid = nargs >= 0 && parse_number(argv[1], UINT32_MAX, &address) &&
                 (nargs < 2 || parse_number(argv[2], ALLTRAX_READ_MEMORY_MAX, &length)) &&
                 (!flags.watch || (parse_number(flags.watch, WATCH_MAX_MS, &watch_ms) &&
                                   watch_ms >= WATCH_MIN_MS));
    if (!valid) {
        fprintf(stderr,
            "Usage: alltrax debug-read [options] <address> [length]\n"
            "\n"
            "Reads length bytes (default 4, at most %d) from RAM 0x20000000-0x2000FFFF,\n"
            "FLASH 0x08000000-0x0801FFFF or the backup registers 0x40006C00-0x40006C13.\n"
            "Numbers are decimal or 0x-prefixed hexadecimal. --watch takes %d-%d ms.\n",
            ALLTRAX_READ_MEMORY_MAX, WATCH_MIN_MS, WATCH_MAX_MS);
        cli_print_options(stderr, CLI_OPT_WATCH);
        return 1;
    }
    if (!alltrax_memory_readable((uint32_t)address, (size_t)length)) {
        fprintf(stderr, "Error: cannot read %llu bytes at 0x%08llX: debug-read is limited to "
                "RAM, FLASH and the backup registers\n", length, address);
        return 1;
    }

    alltrax_controller* ctrl = NULL;
    if (cli_open(&ctrl, false)) return 1;

    uint8_t buf[ALLTRAX_READ_MEMORY_MAX];
    int rc = 1;
    if (!flags.watch) {
        alltrax_error err = alltrax_read_memory(ctrl, (uint32_t)address, (size_t)length, buf);
        if (err)
            cli_error(ctrl, err, "reading memory");
        else {
            print_dump("", (uint32_t)address, buf, (size_t)length);
            rc = 0;
        }
        alltrax_close(ctrl);
        return rc;
    }

    struct sigaction sa = { .sa_handler = sigint_handler }, old;
    sigemptyset(&sa.sa_mask);
    running = 1;
    struct timespec first;
    if (sigaction(SIGINT, &sa, &old) != 0 || clock_gettime(CLOCK_MONOTONIC, &first) != 0) {
        perror("Error: starting the watch");
        alltrax_close(ctrl);
        return 1;
    }
    while (running) {
        struct timespec start;
        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
            perror("Error: reading the clock");
            break;
        }
        alltrax_error err = alltrax_read_memory(ctrl, (uint32_t)address, (size_t)length, buf);
        if (!running) break;
        if (err) {
            cli_error(ctrl, err, "reading memory");
            break;
        }
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "%10.3f s  ",
                 (double)(start.tv_sec - first.tv_sec) +
                 (double)(start.tv_nsec - first.tv_nsec) / 1e9);
        print_dump(prefix, (uint32_t)address, buf, (size_t)length);
        fflush(stdout);
        cli_wait_interval(&start, (int)watch_ms);
    }
    if (!running) {
        printf("Stopped.\n");
        rc = 0;
    }
    sigaction(SIGINT, &old, NULL);
    alltrax_close(ctrl);
    return rc;
}
