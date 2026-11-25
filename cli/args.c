/*
 * args.c — Shared command-line option parsing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"

bool cli_ignore_fw_bounds = false;

/* Debug bypasses are listed last and printed under their own heading. */
static const struct {
    const char* name;
    cli_option_mask bit;
    const char* value;  /* value placeholder for options that take one */
    const char* help;
} options[] = {
    { "--allow-unvalidated-firmware", CLI_OPT_ALLOW_UNVALIDATED, NULL,
      "Write to firmware V0.001-V5.999 not yet validated for writes" },
    { "--throttle", CLI_OPT_THROTTLE, NULL,
      "Let a bulk write include throttle settings (must match the installed throttle)" },
    { "--field-map", CLI_OPT_FIELD_MAP, NULL,
      "Let a bulk write include field-map data (must match the installed motor)" },
    { "--key", CLI_OPT_KEY, "<key>",
      "File key: auto (default), none for an unencrypted file, or <48 hex>:<16 hex>" },
    { "--factory", CLI_OPT_FACTORY, NULL,
      "Read the factory settings page" },
    { "--file", CLI_OPT_FILE, "<f.csv>",
      "Read or write curve points as CSV" },
    { "--preset", CLI_OPT_PRESET, "<name>",
      "Use a built-in preset curve" },
    { "--plot", CLI_OPT_PLOT, NULL,
      "Show an ASCII plot instead of a table" },
    { "--rate", CLI_OPT_MONITOR_RATE, "<ms>",
      "Full monitor polling interval: 100, 1000, or 3000 (default 100)" },
    { "--log", CLI_OPT_MONITOR_LOG, "<file.csv>",
      "Append full monitor samples to a CSV file" },
    { "--debug-no-rules", CLI_OPT_DEBUG_NO_RULES, NULL,
      "Skip the value rules for settings and curves" },
    { "--debug-no-cal", CLI_OPT_DEBUG_NO_CAL, NULL,
      "Skip the CAL/RUN mode bracket" },
    { "--debug-no-verify", CLI_OPT_DEBUG_NO_VERIFY, NULL,
      "Skip FLASH read-back verification" },
    { "--debug-no-post-write", CLI_OPT_DEBUG_NO_POST_WRITE, NULL,
      "Skip automatic calibration clear, date stamp and reset" },
};
#define OPTION_COUNT (sizeof(options) / sizeof(options[0]))
#define DEBUG_OPTIONS (CLI_OPT_DEBUG_NO_RULES | CLI_OPT_DEBUG_NO_CAL | \
                       CLI_OPT_DEBUG_NO_VERIFY | CLI_OPT_DEBUG_NO_POST_WRITE)

static cli_option_mask option_bit(const char* arg)
{
    for (size_t i = 0; i < OPTION_COUNT; i++) {
        if (strcmp(arg, options[i].name) == 0)
            return options[i].bit;
    }
    return 0;
}

static void print_option_group(FILE* out, cli_option_mask allowed)
{
    for (size_t i = 0; i < OPTION_COUNT; i++) {
        if (!(options[i].bit & allowed))
            continue;
        char label[48];
        snprintf(label, sizeof(label), "%s%s%s", options[i].name,
                 options[i].value ? " " : "",
                 options[i].value ? options[i].value : "");
        fprintf(out, "  %-30s %s\n", label, options[i].help);
    }
}

void cli_print_options(FILE* out, cli_option_mask allowed)
{
    if (allowed & ~DEBUG_OPTIONS) {
        fprintf(out, "\nOptions:\n");
        print_option_group(out, allowed & ~DEBUG_OPTIONS);
    }
    if (allowed & DEBUG_OPTIONS) {
        fprintf(out, "\nDebug and development (bypasses safety checks):\n");
        print_option_group(out, allowed & DEBUG_OPTIONS);
    }
}

#define RECONNECT_TIMEOUT_MS 7500u

void cli_wait_for_reconnect(alltrax_controller* ctrl)
{
    fflush(stdout);
    if (alltrax_wait_for_reconnect(ctrl, RECONNECT_TIMEOUT_MS) == ALLTRAX_OK)
        printf("Controller reconnected\n");
    else
        fprintf(stderr, "Warning: controller did not reappear within %.1f seconds "
                "after reset. If using USB passthrough, reconnect the cable.\n",
                RECONNECT_TIMEOUT_MS / 1000.0);
}

void cli_report_write(alltrax_controller* ctrl, const alltrax_write_report* report)
{
    if (report->factory_restored)
        fprintf(stderr, "User settings were invalid; factory settings were "
                "restored before writing.\n");
    if (report->autocal_cleared)
        printf("Cleared throttle auto-calibration\n");
    if (report->reset_sent) {
        printf("Device reset sent (controller will reboot)\n");
        cli_wait_for_reconnect(ctrl);
    }
}

alltrax_write_opts cli_write_opts(const cli_flags* flags,
    alltrax_write_report* report)
{
    return (alltrax_write_opts){
        .skip_cal = flags->debug_no_cal,
        .skip_verify = flags->debug_no_verify,
        .skip_post_write = flags->debug_no_post_write,
        .skip_fw_check = flags->allow_unvalidated_firmware,
        .report = report,
    };
}

int cli_parse_args(int argc, char** argv, const char* command,
    cli_option_mask allowed, int min_args, int max_args, cli_flags* flags)
{
    memset(flags, 0, sizeof(*flags));

    int npos = 0;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (strncmp(arg, "--", 2) != 0) {
            argv[1 + npos++] = argv[i];
            continue;
        }

        cli_option_mask bit = option_bit(arg);
        if (!bit) {
            fprintf(stderr, "Error: %s: unknown option %s\n", command, arg);
            return -1;
        }
        if (!(bit & allowed)) {
            fprintf(stderr, "Error: %s does not accept %s\n", command, arg);
            return -1;
        }

        const char** value = bit == CLI_OPT_KEY ? &flags->key
                           : bit == CLI_OPT_FILE ? &flags->file
                           : bit == CLI_OPT_PRESET ? &flags->preset
                           : bit == CLI_OPT_MONITOR_RATE ? &flags->monitor_rate
                           : bit == CLI_OPT_MONITOR_LOG ? &flags->monitor_log
                           : NULL;
        if (value) {
            if (i + 1 >= argc || strncmp(argv[i + 1], "--", 2) == 0) {
                fprintf(stderr, "Error: %s: %s requires a value\n",
                        command, arg);
                return -1;
            }
            if (*value) {
                fprintf(stderr, "Error: %s: %s given more than once\n",
                        command, arg);
                return -1;
            }
            *value = argv[++i];
            continue;
        }

        switch (bit) {
        case CLI_OPT_ALLOW_UNVALIDATED: flags->allow_unvalidated_firmware = true; break;
        case CLI_OPT_THROTTLE:          flags->throttle = true; break;
        case CLI_OPT_FIELD_MAP:         flags->field_map = true; break;
        case CLI_OPT_FACTORY:           flags->factory = true; break;
        case CLI_OPT_PLOT:              flags->plot = true; break;
        case CLI_OPT_DEBUG_NO_RULES:    flags->debug_no_rules = true; break;
        case CLI_OPT_DEBUG_NO_CAL:      flags->debug_no_cal = true; break;
        case CLI_OPT_DEBUG_NO_VERIFY:   flags->debug_no_verify = true; break;
        case CLI_OPT_DEBUG_NO_POST_WRITE: flags->debug_no_post_write = true; break;
        default: break;
        }
    }

    if (max_args >= 0 && npos > max_args) {
        fprintf(stderr, "Error: %s: unexpected argument %s\n",
                command, argv[1 + max_args]);
        return -1;
    }
    if (npos < min_args) {
        fprintf(stderr, "Error: %s: missing required argument\n", command);
        return -1;
    }
    return npos;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char* text, unsigned char* out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        int hi = hex_digit(text[2 * i]), lo = hex_digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return true;
}

/* Parse "<48 hex digits>:<16 hex digits>". */
static bool parse_key(const char* text, cli_key* out)
{
    const char* colon = strchr(text, ':');
    if (!colon || colon - text != 2 * (long)sizeof(out->key) ||
        strlen(colon + 1) != 2 * sizeof(out->iv))
        return false;
    out->encrypt = true;
    return parse_hex(text, out->key, sizeof(out->key)) &&
           parse_hex(colon + 1, out->iv, sizeof(out->iv));
}

int cli_resolve_key(const char* command, const char* value, const char* env,
    const cli_key* embedded, cli_key* out)
{
    memset(out, 0, sizeof(*out));
    if (value && strcmp(value, "none") == 0)
        return 0;
#ifndef ALLTRAX_HAVE_CRYPTO
    fprintf(stderr, "Error: %s: this build reads and writes only unencrypted files "
            "(built without libcrypto). Use --key none.\n", command);
    return 1;
#endif
    if (value && strcmp(value, "auto") != 0) {
        if (parse_key(value, out)) return 0;
        fprintf(stderr, "Error: %s: --key must be auto, none, or "
                "<48 hex digits>:<16 hex digits>\n", command);
        return 1;
    }

    const char* text = getenv(env);
    if (text && *text) {
        if (parse_key(text, out)) return 0;
        fprintf(stderr, "Error: %s: %s must hold <48 hex digits>:<16 hex digits>\n",
                command, env);
        return 1;
    }
    if (embedded) {
        *out = *embedded;
        out->encrypt = true;
        return 0;
    }
    fprintf(stderr, "Error: %s: no key is built in. Use --key <48 hex digits>:<16 hex digits>, "
            "set %s, or use --key none for an unencrypted file\n",
            command, env);
    return 1;
}

void cli_print_key_status(FILE* out, const char* what, const char* env,
    const cli_key* embedded)
{
#ifdef ALLTRAX_HAVE_CRYPTO
    fprintf(out, "\nBuilt-in %s key: %s. %s=<key>:<iv> supplies a key for auto.\n",
            what, embedded ? "yes" : "no", env);
#else
    (void)env;
    (void)embedded;
    fprintf(out, "\nEncrypted %s files: unavailable (built without libcrypto). "
            "Use --key none.\n", what);
#endif
}
