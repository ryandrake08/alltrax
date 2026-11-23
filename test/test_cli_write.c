/* CLI mutation preflight tests. No USB transport is linked. */
#define _POSIX_C_SOURCE 200809L
#include "helpers.h"
#include "internal.h"
#include "../cli/cli.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

int test_pass_count;
int test_fail_count;
static unsigned open_calls, unexpected_calls;
static bool open_succeeds;
static alltrax_controller fake_ctrl;
static unsigned error_calls, close_calls, transport_calls;
static alltrax_error reported_error;
static char reported_detail[256];

alltrax_error cli_open(alltrax_controller** ctrl, bool writes)
{
    (void)writes;
    open_calls++;
    if (open_succeeds) {
        *ctrl = &fake_ctrl;
        return ALLTRAX_OK;
    }
    return ALLTRAX_ERR_INVALID_ARG;
}

void cli_error(alltrax_controller* ctrl, alltrax_error err, const char* action)
{
    (void)action;
    error_calls++;
    reported_error = err;
    snprintf(reported_detail, sizeof(reported_detail), "%s", ctrl->error_detail);
    if (ctrl != &fake_ctrl || !open_succeeds) unexpected_calls++;
}

void alltrax_close(alltrax_controller* ctrl)
{
    close_calls++;
    if (ctrl != &fake_ctrl || !open_succeeds) unexpected_calls++;
}

/* Config export references this transport-owned formatter; these preflight
 * tests never export or invoke it. */
const char* alltrax_strerror(alltrax_error err)
{
    (void)err;
    unexpected_calls++;
    return "unexpected error formatting";
}

/* Simulated device. Read requests return zero-filled payloads, except the
 * GoodSet markers and the 4 little-endian bytes of `value` at `value_addr`;
 * read reply `fail_read_at` (1-based) fails. Mutations are counted. Without
 * `writable` they fail; with it, the VARSET page lives in `page` (reads,
 * writes, erase), RunMode is tracked, other writes are echoed, and resets are
 * counted. FACTSET reads come from `factory` (GoodSet from fact_goodset). */
static struct {
    bool enabled, writable;
    uint8_t request[PACKET_SIZE];
    unsigned reads, mutations, fail_read_at, erases, resets;
    uint16_t var_goodset, fact_goodset;
    uint32_t value_addr, value;
    uint8_t page[VARSET_SIZE];
    uint8_t factory[FLASH_PAGE_SIZE];
    uint8_t mode;
} device;

static uint32_t request_addr(void)
{
    const uint8_t* a = device.request + 4;
    return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
           ((uint32_t)a[2] << 8) | a[3];
}

/* Use the real controller implementation; transport calls fail unless the
 * simulated device is enabled. */
alltrax_error transport_write(alltrax_controller* ctrl,
    const uint8_t data[PACKET_SIZE])
{
    (void)ctrl;
    transport_calls++;
    if (!device.enabled)
        return ALLTRAX_ERR_USB;
    memcpy(device.request, data, PACKET_SIZE);
    if (data[0] == REPORT_ID_READ)
        return ALLTRAX_OK;
    device.mutations++;
    if (!device.writable)
        return ALLTRAX_ERR_USB;

    uint32_t addr = request_addr();
    if (data[0] == REPORT_ID_SPECIAL) {
        if (addr == FUNC_RESET) {
            device.resets++;
        } else if (data[11] == VARSET_PAGE) {
            memset(device.page, 0xFF, sizeof(device.page));
            device.erases++;
        }
        return ALLTRAX_OK;
    }
    if (addr == ADDR_RUN_MODE)
        device.mode = data[8];
    else if (addr >= VARSET_ADDR && addr - VARSET_ADDR + data[1] <= VARSET_SIZE)
        memcpy(device.page + (addr - VARSET_ADDR), data + 8, data[1]);
    return ALLTRAX_OK;
}

/* A 48V XCT on write-validated firmware with every throttle type allowed. */
static uint8_t identity_byte(uint32_t a)
{
    static const char model[] = "XCT48400-DCS";
    const struct { uint32_t addr; uint32_t value; } words[] = {
        { ADDR_CONTROLLER_INFO + 0x24, 5002 }, { ADDR_CONTROLLER_INFO + 0x28, 5005 },
        { ADDR_HARDWARE_CONFIG, 48 | (400u << 16) },
        { ADDR_HARDWARE_CONFIG + 16, 0x7FF },
        { ADDR_BOOT_REV, 5002 }, { ADDR_PRGM_REV, fake_ctrl.program_rev },
    };
    if (a >= ADDR_CONTROLLER_INFO && a < ADDR_CONTROLLER_INFO + sizeof(model) - 1)
        return (uint8_t)model[a - ADDR_CONTROLLER_INFO];
    for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        if (a - words[w].addr < 4)
            return (uint8_t)(words[w].value >> ((a - words[w].addr) * 8));
    }
    return 0;
}

alltrax_error transport_drain(alltrax_controller* ctrl)
{
    (void)ctrl;
    return ALLTRAX_OK;
}

alltrax_error transport_read(alltrax_controller* ctrl, uint8_t buf[PACKET_SIZE])
{
    (void)ctrl;
    transport_calls++;
    if (!device.enabled || ++device.reads == device.fail_read_at)
        return ALLTRAX_ERR_USB;
    memset(buf, 0, PACKET_SIZE);
    buf[0] = RESPONSE_ID;
    if (device.request[0] == REPORT_ID_SPECIAL) {
        /* Captured successful page-erase reply */
        const uint8_t erase_reply[] = { 0x04, 0x02, 0, 0, 0x03, 0x01, 0, 0, 0xD4, 0x29 };
        memcpy(buf, erase_reply, sizeof(erase_reply));
        return ALLTRAX_OK;
    }
    buf[1] = device.request[1];
    buf[4] = RESPONSE_TYPE_RW;
    if (device.request[0] == REPORT_ID_WRITE) {
        memcpy(buf + 8, device.request + 8, buf[1]);  /* Write echo */
        return ALLTRAX_OK;
    }

    uint32_t addr = request_addr();
    for (uint32_t i = 0; i < buf[1]; i++) {
        uint32_t a = addr + i;
        buf[8 + i] = identity_byte(a);
        if (device.writable && a >= VARSET_ADDR && a - VARSET_ADDR < VARSET_SIZE)
            buf[8 + i] = device.page[a - VARSET_ADDR];
        else if (device.writable && a == ADDR_RUN_MODE)
            buf[8 + i] = device.mode;
        else if (!device.writable && a < VARSET_ADDR + 2 && a >= VARSET_ADDR)
            buf[8 + i] = (uint8_t)(device.var_goodset >> ((a - VARSET_ADDR) * 8));
        else if (a >= FACTSET_ADDR && a < FACTSET_ADDR + 2)
            buf[8 + i] = (uint8_t)(device.fact_goodset >> ((a - FACTSET_ADDR) * 8));
        else if (a >= FACTSET_ADDR && a - FACTSET_ADDR < FLASH_PAGE_SIZE)
            buf[8 + i] = device.factory[a - FACTSET_ADDR];
        if (device.value_addr && a - device.value_addr < 4)
            buf[8 + i] = (uint8_t)(device.value >> ((a - device.value_addr) * 8));
    }
    return ALLTRAX_OK;
}

static int test_valid_strings_reach_open(void)
{
    const char* names[] = { "Throttle_Type_Name", "F_Table_Name" };
    const char* values[] = { "123", "", "Custom name", "ABCDEFGHIJKLMNOP" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
            char arg[80];
            snprintf(arg, sizeof(arg), "%s=%s", names[i], values[j]);
            char* argv[] = { "write", arg, "--field-map" };
            open_calls = unexpected_calls = 0;
            ASSERT_EQ(cmd_write(3, argv), 1); /* The fake open always fails. */
            ASSERT_EQ(open_calls, 1);
            ASSERT_EQ(unexpected_calls, 0);
        }
    }
    return 0;
}

static int test_invalid_strings_rejected_before_open(void)
{
    const char* bad[] = {
        "Throttle_Type_Name=ABCDEFGHIJKLMNOPQ",             /* 17 > 16 */
        "F_Table_Name=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456",   /* 33 > 32 */
        "F_Table_Name=caf\xc3\xa9",                         /* UTF-8 */
        "F_Table_Name=tab\there",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        for (int pos = 0; pos < 2; pos++) {
            char arg[80], numeric[] = "HPD=1";
            snprintf(arg, sizeof(arg), "%s", bad[i]);
            char* argv[] = { "write", pos ? numeric : arg, pos ? arg : numeric };
            open_calls = unexpected_calls = 0;
            ASSERT_EQ(cmd_write(3, argv), 1);
            ASSERT_EQ(open_calls, 0);
            ASSERT_EQ(unexpected_calls, 0);
        }
    }
    return 0;
}

static int test_numeric_and_bool_assignments_reach_open(void)
{
    char numeric[] = "N_Speed_Limit=4000";
    char boolean[] = "HPD=Yes";
    char* argv[] = { "write", numeric, boolean };
    open_calls = unexpected_calls = 0;
    errno = ERANGE; /* A prior conversion must not poison a valid parse. */
    ASSERT_EQ(cmd_write(3, argv), 1); /* The fake open always fails. */
    ASSERT_EQ(open_calls, 1);
    ASSERT_EQ(unexpected_calls, 0);
    return 0;
}

static int test_read_only_assignments_use_library_diagnostics(void)
{
    const char* names[] = { "Global_Shutdown", "Var_GoodSet", "SerialNum" };
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        for (int pos = 0; pos < 3; pos++) {
            char read_only[80];
            snprintf(read_only, sizeof(read_only), "%s=1", names[n]);
            char writable[] = "HPD=Yes";
            char* argv[] = {
                "write", pos == 1 ? writable : read_only,
                pos == 1 ? read_only : writable,
            };
            fake_ctrl = (alltrax_controller) {
                .vid = ALLTRAX_VID, .pid = ALLTRAX_PID_XCT,
                .controller_type = ALLTRAX_CONTROLLER_XCT, .allow_writes = true,
                .boot_rev = 5002, .program_rev = 5005,
            };
            open_calls = unexpected_calls = error_calls = close_calls = transport_calls = 0;
            open_succeeds = true;
            int rc = cmd_write(pos == 2 ? 2 : 3, argv);
            open_succeeds = false;
            ASSERT_EQ(rc, 1);
            ASSERT_EQ(open_calls, 1);
            ASSERT_EQ(error_calls, 1);
            ASSERT_EQ(reported_error, ALLTRAX_ERR_INVALID_ARG);
            ASSERT_TRUE(strstr(reported_detail, names[n]) != NULL);
            ASSERT_TRUE(strstr(reported_detail, "read-only") != NULL);
            ASSERT_EQ(close_calls, 1);
            ASSERT_EQ(transport_calls, 0);
            ASSERT_EQ(unexpected_calls, 0);
        }
    }
    return 0;
}

static int test_invalid_numeric_text_rejected_before_open(void)
{
    const char* invalid[] = {
        "", " ", "nan", "inf", "-inf", "1e100", "-1e100", "1e9999",
        "-1e9999", "1e-9999", "1e-320", "0x1p-99999", "28.1junk", "6553.55", "-0.1",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        for (int pos = 0; pos < 3; pos++) {
            char bad[80];
            snprintf(bad, sizeof(bad), "LoBat_Vlim=%s", invalid[i]);
            char good[] = "HPD=Yes";
            char* argv[] = { "write", pos == 1 ? good : bad, pos == 1 ? bad : good };
            open_calls = transport_calls = 0;
            ASSERT_EQ(cmd_write(pos == 2 ? 2 : 3, argv), 1);
            ASSERT_EQ(open_calls, 0);
            ASSERT_EQ(transport_calls, 0);
        }
    }
    return 0;
}

static int test_throttle_enum_assignments_use_shared_validation(void)
{
    const char* invalid[] = { "12", "33", "255", "0.49", "1.5", "11.01", "-0.1" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char arg[80], good[] = "HPD=Yes";
        snprintf(arg, sizeof(arg), "Throttle_Type=%s", invalid[i]);
        char* argv[] = { "write", good, arg };
        open_calls = transport_calls = 0;
        ASSERT_EQ(cmd_write(3, argv), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(transport_calls, 0);
    }
    for (int type = 0; type <= 11; type++) {
        char arg[80];
        snprintf(arg, sizeof(arg), "Throttle_Type=%d.0", type);
        char* argv[] = { "write", arg };
        open_calls = 0;
        ASSERT_EQ(cmd_write(2, argv), 1); /* Valid input reaches the failing fake open. */
        ASSERT_EQ(open_calls, 1);
    }
    return 0;
}

static int test_invalid_curve_arguments_rejected_before_open(void)
{
    const char* bad[] = {
        "nan,0", "0,inf", "200,0", "0,200", "-1,0", "0,-1",
        "1e9999,0", "0,1e-9999", ",0", "0,", "0,0junk",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char arg[80];
        snprintf(arg, sizeof(arg), "%s", bad[i]);
        char* argv[] = { "curve", "set", "linearization", "0,0", arg };
        open_calls = transport_calls = 0;
        ASSERT_EQ(cmd_curve(5, argv), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(transport_calls, 0);
    }
    char* argv[20] = { "curve", "set", "linearization" };
    for (int i = 3; i < 20; i++) argv[i] = "0,0";
    open_calls = 0;
    ASSERT_EQ(cmd_curve(20, argv), 1);
    ASSERT_EQ(open_calls, 0);
    return 0;
}

/* Return -1 for fixture setup errors so they cannot masquerade as rejection. */
static int run_curve_file(const char* contents, bool config)
{
    char path[] = "/tmp/alltrax-curve-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    FILE* f = fdopen(fd, "w");
    if (!f) { close(fd); remove(path); return -1; }
    bool wrote = fputs(contents, f) >= 0;
    int closed = fclose(f);
    if (!wrote || closed) { remove(path); return -1; }
    int rc;
    if (config) {
#ifdef TEST_CONFIG
        char* argv[] = { "config", "load", "--no-crypt", path };
        rc = cmd_config(4, argv);
#else
        rc = -1;
#endif
    } else {
        char* argv[] = { "curve", "set", "linearization", "--file", path };
        rc = cmd_curve(5, argv);
    }
    remove(path);
    return rc;
}

static int test_curve_csv_validation(void)
{
    const char* bad[] = {
        "nan,0\n", "0,inf\n", "200,0\n", "0,200\n", "-1,0\n",
        "0,-1\n", "1e9999,0\n", "0,1e-9999\n", "0,0junk\n", "0,\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char file[160];
        snprintf(file, sizeof(file), "0,0\n%s", bad[i]);
        open_calls = transport_calls = 0;
        ASSERT_EQ(run_curve_file(file, false), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(transport_calls, 0);
    }
    char too_many[128] = "";
    for (int i = 0; i < 17; i++) strcat(too_many, "0,0\n");
    open_calls = 0;
    ASSERT_EQ(run_curve_file(too_many, false), 1);
    ASSERT_EQ(open_calls, 0);
    ASSERT_EQ(run_curve_file("# padded endpoints\n0,0\n100,100\n", false), 1);
    ASSERT_EQ(open_calls, 1); /* Valid input reaches the fake open, which fails. */
    return 0;
}

static int test_curve_presets_reach_open(void)
{
    for (size_t i = 0; i < alltrax_curve_preset_count(); i++) {
        const alltrax_curve_preset* preset = alltrax_curve_preset_by_index(i);
        char type[32], name[32];
        snprintf(type, sizeof(type), "%s", preset->curve_type);
        snprintf(name, sizeof(name), "%s", preset->name);
        char* argv[] = { "curve", "set", type, "--preset", name };
        open_calls = 0;
        ASSERT_EQ(cmd_curve(5, argv), 1);
        ASSERT_EQ(open_calls, 1);
    }
    return 0;
}

#ifdef TEST_CONFIG
static const char zeros[] = "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0";

static int test_config_curves_rejected_before_any_settings_write(void)
{
    const char* bad[] = { "8190", "-1", "32768", "nan", "999999999999999999999999" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char values[128], xml[1024];
        snprintf(values, sizeof(values), "%s,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0", bad[i]);
        /* A valid scalar and first curve must not be applied before a bad
         * second curve is validated. */
        snprintf(xml, sizeof(xml),
            "<XCT><Settings><HPD>1</HPD>"
            "<Lin_Table_Pointer>%s</Lin_Table_Pointer><Lin_Table_Data>%s</Lin_Table_Data>"
            "<V_Table_Pointer>%s</V_Table_Pointer><V_Table_Data>%s</V_Table_Data>"
            "</Settings></XCT>", zeros, zeros, zeros, values);
        open_calls = transport_calls = 0;
        ASSERT_EQ(run_curve_file(xml, true), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(transport_calls, 0);
    }
    const char* incomplete =
        "<XCT><Settings><HPD>1</HPD><Lin_Table_Pointer>"
        "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0</Lin_Table_Pointer></Settings></XCT>";
    open_calls = 0;
    ASSERT_EQ(run_curve_file(incomplete, true), 1);
    ASSERT_EQ(open_calls, 0);
    return 0;
}

static int test_valid_curve_config_reaches_open(void)
{
    char xml[512];
    snprintf(xml, sizeof(xml),
        "<XCT><Settings><HPD>1</HPD><Lin_Table_Pointer>%s</Lin_Table_Pointer>"
        "<Lin_Table_Data>%s</Lin_Table_Data></Settings></XCT>", zeros, zeros);
    open_calls = 0;
    ASSERT_EQ(run_curve_file(xml, true), 1);
    ASSERT_EQ(open_calls, 1);
    return 0;
}

static int test_invalid_throttle_config_rejected_before_transport(void)
{
    const char* xml = "<XCT><Settings><HPD>1</HPD>"
                      "<Throttle_Type>33</Throttle_Type></Settings></XCT>";
    fake_ctrl = (alltrax_controller) {
        .vid = ALLTRAX_VID, .pid = ALLTRAX_PID_XCT,
        .controller_type = ALLTRAX_CONTROLLER_XCT, .allow_writes = true,
        .boot_rev = 5002, .program_rev = 5005,
    };
    open_calls = transport_calls = error_calls = close_calls = unexpected_calls = 0;
    open_succeeds = true;
    int rc = run_curve_file(xml, true);
    open_succeeds = false;
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(open_calls, 1);
    ASSERT_EQ(transport_calls, 0);
    ASSERT_EQ(error_calls, 1);
    ASSERT_EQ(close_calls, 1);
    ASSERT_EQ(reported_error, ALLTRAX_ERR_INVALID_ARG);
    ASSERT_TRUE(strstr(reported_detail, "Throttle_Type") != NULL);
    ASSERT_EQ(unexpected_calls, 0);
    return 0;
}
#endif

static int test_options_accepted_before_and_after_positionals(void)
{
    const unsigned allowed = CLI_OPT_DEBUG_NO_CAL | CLI_OPT_ALLOW_UNVALIDATED |
                             CLI_OPT_RESET | CLI_OPT_FILE;
    char* argv[] = {
        "set", "--reset", "linearization", "--debug-no-cal", "-1,0",
        "--file", "lin.csv", "-", "--allow-unvalidated-firmware",
    };
    cli_flags flags;
    ASSERT_EQ(cli_parse_args(9, argv, "curve set", allowed, 1, -1, &flags), 3);
    ASSERT_STR_EQ(argv[1], "linearization");
    ASSERT_STR_EQ(argv[2], "-1,0");
    ASSERT_STR_EQ(argv[3], "-");
    ASSERT_TRUE(flags.reset);
    ASSERT_TRUE(flags.debug_no_cal);
    ASSERT_TRUE(flags.allow_unvalidated_firmware);
    ASSERT_FALSE(flags.debug_no_verify);
    ASSERT_STR_EQ(flags.file, "lin.csv");
    ASSERT_NULL(flags.preset);
    return 0;
}

static int test_invalid_options_and_argument_counts_rejected(void)
{
    const unsigned allowed = CLI_OPT_RESET | CLI_OPT_FILE;
    struct { int argc; char* argv[5]; int min, max; } bad[] = {
        { 2, { "cmd", "--bogus" }, 0, -1 },            /* unknown */
        { 2, { "cmd", "--debug-no-goodset" }, 0, -1 }, /* not accepted */
        { 2, { "cmd", "--file" }, 0, -1 },             /* missing value */
        { 3, { "cmd", "--file", "--reset" }, 0, -1 },  /* option as value */
        { 5, { "cmd", "--file", "a", "--file", "b" }, 0, -1 }, /* repeated */
        { 3, { "cmd", "one", "two" }, 1, 1 },          /* extra */
        { 2, { "cmd", "--reset" }, 1, 1 },             /* missing */
        { 2, { "cmd", "--" }, 0, -1 },                 /* bare -- */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cli_flags flags;
        ASSERT_EQ(cli_parse_args(bad[i].argc, bad[i].argv, "cmd", allowed,
                                 bad[i].min, bad[i].max, &flags), -1);
    }
    char* none[] = { "cmd" };
    cli_flags flags;
    ASSERT_EQ(cli_parse_args(1, none, "cmd", 0, 0, 0, &flags), 0);
    return 0;
}

static int test_write_options_map_to_library_options(void)
{
    char* argv[] = {
        "write", "--debug-no-cal", "--debug-no-verify", "--debug-no-goodset",
        "--allow-unvalidated-firmware", "--no-voltage-link",
    };
    cli_flags flags;
    ASSERT_EQ(cli_parse_args(6, argv, "write", ~0u, 0, -1, &flags), 0);
    alltrax_write_report report;
    alltrax_write_opts opts = cli_write_opts(&flags, &report);
    ASSERT_TRUE(opts.report == &report);
    ASSERT_TRUE(opts.skip_cal && opts.skip_verify && opts.skip_goodset &&
                opts.skip_fw_check && opts.skip_voltage_link);
    return 0;
}

static int test_bad_command_options_rejected_before_open(void)
{
    struct { int argc; char* argv[7]; } bad[] = {
        { 6, { "curve", "set", "linearization", "--preset", "linear", "--bogus" } },
        { 6, { "curve", "set", "linearization", "--preset", "linear", "0,0" } },
        { 7, { "curve", "set", "linearization", "--file", "x.csv", "--preset", "linear" } },
        { 3, { "curve", "set", "linearization" } },
        { 4, { "curve", "set", "linearization", "--preset" } },
        { 4, { "curve", "reset", "linearization", "extra" } },
        { 4, { "curve", "reset", "linearization", "--bogus" } },
        { 4, { "curve", "reset", "linearization", "--plot" } },
        { 4, { "curve", "get", "linearization", "extra" } },
        { 4, { "curve", "diff", "linearization", "--reset" } },
        { 3, { "curve", "list", "extra" } },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        open_calls = transport_calls = 0;
        ASSERT_EQ(cmd_curve(bad[i].argc, bad[i].argv), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(transport_calls, 0);
    }
    char* reset_extra[] = { "reset", "extra" };
    char* reset_bogus[] = { "reset", "--reset" };
    char assign[] = "HPD=1";
    char* write_bogus[] = { "write", assign, "--plot" };
    open_calls = 0;
    ASSERT_EQ(cmd_reset(2, reset_extra), 1);
    ASSERT_EQ(cmd_reset(2, reset_bogus), 1);
    ASSERT_EQ(cmd_write(3, write_bogus), 1);
    ASSERT_EQ(open_calls, 0);
#ifdef TEST_CONFIG
    char* save_extra[] = { "config", "save", "out.xml", "extra" };
    char* load_bogus[] = { "config", "load", "in.xml", "--plot" };
    ASSERT_EQ(cmd_config(4, save_extra), 1);
    ASSERT_EQ(cmd_config(4, load_bogus), 1);
    ASSERT_EQ(open_calls, 0);
#endif
    return 0;
}

/* Firmware V5.004 is readable but not write-validated, so the write gate
 * returns ALLTRAX_ERR_FIRMWARE unless --allow-unvalidated-firmware reached the write
 * options; with it, the first device transfer fails with ALLTRAX_ERR_USB. */
static alltrax_error run_on_unvalidated_firmware(int (*cmd)(int, char**),
    int argc, char** argv)
{
    fake_ctrl = (alltrax_controller) {
        .vid = ALLTRAX_VID, .pid = ALLTRAX_PID_XCT,
        .controller_type = ALLTRAX_CONTROLLER_XCT, .allow_writes = true,
        .boot_rev = 5002, .program_rev = 5004,
    };
    open_calls = error_calls = close_calls = transport_calls = 0;
    reported_error = ALLTRAX_OK;
    open_succeeds = true;
    int rc = cmd(argc, argv);
    open_succeeds = false;
    return rc == 1 && error_calls == 1 ? reported_error : ALLTRAX_OK;
}

static int test_fw_flag_reaches_write_options_in_any_position(void)
{
    for (int after = 0; after <= 1; after++) {
        char* set[] = {
            "curve", "set", after ? "linearization" : "--allow-unvalidated-firmware",
            "--preset", "linear", after ? "--allow-unvalidated-firmware" : "linearization",
        };
        ASSERT_EQ(run_on_unvalidated_firmware(cmd_curve, 6, set), ALLTRAX_ERR_USB);
        char* set_default[] = { "curve", "set", "linearization", "--preset", "linear" };
        ASSERT_EQ(run_on_unvalidated_firmware(cmd_curve, 5, set_default),
                  ALLTRAX_ERR_FIRMWARE);
        ASSERT_EQ(transport_calls, 0);

        char assign[] = "HPD=1";
        char* write[] = { "write", after ? assign : "--allow-unvalidated-firmware",
                          after ? "--allow-unvalidated-firmware" : assign };
        ASSERT_EQ(run_on_unvalidated_firmware(cmd_write, 3, write), ALLTRAX_ERR_USB);
    }
    char* reset[] = { "reset", "--allow-unvalidated-firmware" };
    ASSERT_EQ(run_on_unvalidated_firmware(cmd_reset, 2, reset), ALLTRAX_ERR_USB);
    char* reset_default[] = { "reset" };
    ASSERT_EQ(run_on_unvalidated_firmware(cmd_reset, 1, reset_default),
              ALLTRAX_ERR_FIRMWARE);
    ASSERT_EQ(transport_calls, 0);
    return 0;
}

#ifdef TEST_CONFIG
/* Run `config save [--no-crypt] <path>` against the simulated device. */
static int run_config_save_as(const char* path, uint32_t program_rev,
    bool encrypt)
{
    char file[256];
    snprintf(file, sizeof(file), "%s", path);
    char* argv[] = { "config", "save", encrypt ? file : "--no-crypt", file };
    fake_ctrl = (alltrax_controller) {
        .vid = ALLTRAX_VID, .pid = ALLTRAX_PID_XCT,
        .controller_type = ALLTRAX_CONTROLLER_XCT,
        .boot_rev = 5002, .program_rev = program_rev,
    };
    open_calls = error_calls = close_calls = transport_calls = 0;
    device.enabled = true;
    device.reads = device.mutations = 0;
    open_succeeds = true;
    int rc = cmd_config(encrypt ? 3 : 4, argv);
    open_succeeds = false;
    device.enabled = false;
    return rc;
}

static int run_config_save(const char* path, uint32_t program_rev)
{
    return run_config_save_as(path, program_rev, false);
}

/* Returns the file contents (caller frees), or NULL if it cannot be read. */
static char* read_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    static char data[65536];
    size_t n = fread(data, 1, sizeof(data) - 1, f);
    fclose(f);
    data[n] = '\0';
    return strdup(data);
}

static int make_temp_path(char path[64])
{
    snprintf(path, 64, "/tmp/alltrax-save-test-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

static int write_file(const char* path, const char* contents)
{
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    bool ok = fputs(contents, f) >= 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

static int test_config_save_includes_every_group_and_curve(void)
{
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    memset(&device, 0, sizeof(device));
    ASSERT_EQ(run_config_save(path, 5005), 0);
    ASSERT_EQ(device.mutations, 0);
    char* xml = read_file(path);
    remove(path);
    ASSERT_TRUE(xml != NULL);
    const char* groups[] = {
        "Voltage", "Normal_User", "User_1", "User_2", "Other_Settings",
        "Throttle", "Field",
    };
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        char node[64];
        snprintf(node, sizeof(node), "<Node-%s>", groups[i]);
        ASSERT_TRUE(strstr(xml, node) != NULL);
    }
    const char* curves[] = {
        "Lin_Table_Pointer", "Lin_Table_Data", "V_Table_Pointer",
        "V_Table_Data", "I_Table_Pointer", "I_Table_Data", "F_Table_Pointer",
        "F_Table_Data", "Gen_Field_Pointer", "Gen_Field_Data",
    };
    for (size_t i = 0; i < sizeof(curves) / sizeof(curves[0]); i++) {
        char node[64];
        snprintf(node, sizeof(node), "<%s>", curves[i]);
        ASSERT_TRUE(strstr(xml, node) != NULL);
    }
    free(xml);
    return 0;
}

static int test_config_save_any_read_failure_preserves_destination(void)
{
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    memset(&device, 0, sizeof(device));
    ASSERT_EQ(run_config_save(path, 5005), 0);
    unsigned total_reads = device.reads;
    ASSERT_TRUE(total_reads > 2);

    const char* previous = "previous backup";
    for (unsigned n = 1; n <= total_reads; n++) {
        ASSERT_EQ(write_file(path, previous), 0);
        device.fail_read_at = n;
        ASSERT_EQ(run_config_save(path, 5005), 1);
        ASSERT_EQ(device.reads, n); /* The save stops at the failed read. */
        ASSERT_EQ(error_calls, 1);
        ASSERT_EQ(reported_error, ALLTRAX_ERR_USB);
        char* contents = read_file(path);
        ASSERT_TRUE(contents != NULL);
        ASSERT_STR_EQ(contents, previous);
        free(contents);
    }
    remove(path);

    /* A failed save does not create a missing destination. */
    device.fail_read_at = total_reads;
    ASSERT_EQ(run_config_save(path, 5005), 1);
    ASSERT_EQ(access(path, F_OK), -1);
    device.fail_read_at = 0;
    return 0;
}

static int test_config_save_rejects_invalid_settings(void)
{
    const uint16_t factory[] = { 0x0000, 0xFFFF };
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    ASSERT_EQ(write_file(path, "previous backup"), 0);
    for (size_t i = 0; i < sizeof(factory) / sizeof(factory[0]); i++) {
        memset(&device, 0, sizeof(device));
        device.var_goodset = 0xFFFF;
        device.fact_goodset = factory[i];
        ASSERT_EQ(run_config_save(path, 5005), 1);
        ASSERT_EQ(device.reads, 2); /* Only the two GoodSet markers. */
        ASSERT_EQ(error_calls, 0);
        char* contents = read_file(path);
        ASSERT_TRUE(contents != NULL);
        ASSERT_STR_EQ(contents, "previous backup");
        free(contents);
    }
    remove(path);
    return 0;
}

static int test_config_save_on_unreadable_firmware_preserves_destination(void)
{
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    ASSERT_EQ(write_file(path, "previous backup"), 0);
    memset(&device, 0, sizeof(device));
    ASSERT_EQ(run_config_save(path, 6001), 1);
    ASSERT_EQ(transport_calls, 0);
    ASSERT_EQ(error_calls, 1);
    ASSERT_EQ(reported_error, ALLTRAX_ERR_FIRMWARE);
    char* contents = read_file(path);
    remove(path);
    ASSERT_TRUE(contents != NULL);
    ASSERT_STR_EQ(contents, "previous backup");
    free(contents);
    return 0;
}

static int test_config_save_encrypts_before_writing(void)
{
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    memset(&device, 0, sizeof(device));
    ASSERT_EQ(run_config_save_as(path, 5005, true), 0);
    struct stat st;
    ASSERT_EQ(stat(path, &st), 0);
    char* contents = read_file(path);
    remove(path);
    ASSERT_TRUE(contents != NULL);
    ASSERT_TRUE(st.st_size > 0);
    ASSERT_EQ(st.st_size % 8, 0); /* Whole 3DES blocks */
    ASSERT_NULL(strstr(contents, "<XCT"));
    free(contents);
    return 0;
}

/* Run the save with a file size limit (bytes). */
static int run_config_save_size_limited(const char* path, bool encrypt,
    rlim_t limit)
{
    struct rlimit saved;
    if (getrlimit(RLIMIT_FSIZE, &saved) != 0) return -1;
    struct rlimit limited = saved;
    limited.rlim_cur = limit;
    void (*old_handler)(int) = signal(SIGXFSZ, SIG_IGN);
    if (setrlimit(RLIMIT_FSIZE, &limited) != 0) return -1;
    int rc = run_config_save_as(path, 5005, encrypt);
    setrlimit(RLIMIT_FSIZE, &saved);
    signal(SIGXFSZ, old_handler);
    return rc;
}

/* A 64-byte limit fails the first write. A limit one byte short of the full
 * file lets fwrite() succeed and fails only the final flush in fclose(). */
static int test_config_save_write_failure_removes_partial_file(void)
{
    for (int encrypt = 0; encrypt <= 1; encrypt++) {
        char path[64];
        ASSERT_EQ(make_temp_path(path), 0);
        memset(&device, 0, sizeof(device));
        ASSERT_EQ(run_config_save_as(path, 5005, encrypt != 0), 0);
        struct stat st;
        ASSERT_EQ(stat(path, &st), 0);
        const rlim_t limits[] = { 64, (rlim_t)st.st_size - 1 };
        for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
            ASSERT_EQ(write_file(path, "previous backup"), 0);
            ASSERT_EQ(run_config_save_size_limited(path, encrypt != 0,
                                                   limits[i]), 1);
            ASSERT_EQ(access(path, F_OK), -1);
            ASSERT_EQ(device.mutations, 0);
        }
    }
    return 0;
}

static int test_config_save_write_failure_keeps_device_paths(void)
{
    if (access("/dev/full", W_OK) != 0)
        return 0; /* /dev/full is Linux-specific. */
    memset(&device, 0, sizeof(device));
    ASSERT_EQ(run_config_save("/dev/full", 5005), 1);
    struct stat st;
    ASSERT_EQ(stat("/dev/full", &st), 0);
    ASSERT_TRUE(S_ISCHR(st.st_mode));
    return 0;
}

#endif

/* Writable simulated device whose settings page holds a valid value for every
 * VARSET variable (its minimum bound, or 0), consistent voltage limits, and
 * space-filled strings. */
static void writable_device_reset(void)
{
    memset(&device, 0, sizeof(device));
    device.writable = true;
    for (int g = 0; g < ALLTRAX_VARS_GROUP_COUNT; g++) {
        size_t n;
        const alltrax_var_def* defs = alltrax_get_var_defs((alltrax_var_group)g, &n);
        for (size_t i = 0; defs && i < n; i++) {
            const alltrax_var_def* var = &defs[i];
            if (!var->is_flash || var->address < VARSET_ADDR ||
                var->address >= VARSET_ADDR + VARSET_SIZE)
                continue;
            uint8_t* dst = device.page + (var->address - VARSET_ADDR);
            if (var->type == ALLTRAX_TYPE_STRING) {
                alltrax_encode_string(var, "", dst);
            } else {
                double min, max;
                alltrax_var_display_bounds(var, &min, &max);
                alltrax_encode_var(var, isnan(min) ? 0 : min, dst);
            }
        }
    }
    const struct { const char* name; double volts; } limits[] = {
        { "AnalogInputs_Threshold", 18 }, { "LoBat_Vlim", 20 },
        { "HiBat_Vlim", 60 }, { "BMS_Missing_HiBat_Vlim", 50 },
    };
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        const alltrax_var_def* var = alltrax_find_var(limits[i].name);
        alltrax_encode_var(var, limits[i].volts,
                           device.page + (var->address - VARSET_ADDR));
    }
    device.page[0] = device.page[1] = 0;  /* Var_GoodSet valid */
}

static int run_writable(int (*cmd)(int, char**), int argc, char** argv)
{
    fake_ctrl = (alltrax_controller) {
        .vid = ALLTRAX_VID, .pid = ALLTRAX_PID_XCT,
        .controller_type = ALLTRAX_CONTROLLER_XCT, .allow_writes = true,
        .boot_rev = 5002, .program_rev = 5005,
    };
    open_calls = error_calls = close_calls = transport_calls = 0;
    device.enabled = true;
    open_succeeds = true;
    int rc = cmd(argc, argv);
    open_succeeds = false;
    device.enabled = false;
    return rc;
}

#ifdef TEST_CONFIG
static void set_page_string(const char* name, const char* text)
{
    const alltrax_var_def* var = alltrax_find_var(name);
    alltrax_encode_string(var, text, device.page + (var->address - VARSET_ADDR));
}

static int test_config_round_trip_restores_strings_in_one_erase(void)
{
    const struct { const char* field; const char* throttle; } cases[] = {
        { "MY FIELD MAP", "" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", "ABCDEFGHIJKLMNOP" },  /* max */
        { "", "Custom ~!@#" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        writable_device_reset();
        set_page_string("F_Table_Name", cases[i].field);
        set_page_string("Throttle_Type_Name", cases[i].throttle);
        uint8_t original[VARSET_SIZE];
        memcpy(original, device.page, sizeof(original));

        char path[64];
        ASSERT_EQ(make_temp_path(path), 0);
        ASSERT_EQ(run_config_save(path, 5005), 0);

        set_page_string("F_Table_Name", "CHANGED");
        set_page_string("Throttle_Type_Name", "CHANGED");
        device.erases = device.mutations = 0;
        char* argv[] = { "config", "load", "--no-crypt", path, "--field-map" };
        int rc = run_writable(cmd_config, 5, argv);
        remove(path);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(device.erases, 1);
        ASSERT_EQ(device.mode, RUN_MODE_RUN);
        ASSERT_MEM_EQ(device.page, original, sizeof(original));
    }
    return 0;
}

static int test_config_load_reset_flag_sends_one_reset(void)
{
    for (int fail = 0; fail <= 1; fail++) {
        writable_device_reset();
        char path[64];
        ASSERT_EQ(make_temp_path(path), 0);
        ASSERT_EQ(run_config_save(path, 5005), 0);
        if (fail)
            device.fact_goodset = 0xFFFF;
        device.erases = device.resets = 0;
        char* argv[] = { "config", "load", "--no-crypt", path, "--reset" };
        int rc = run_writable(cmd_config, 5, argv);
        remove(path);
        ASSERT_EQ(rc, fail ? 1 : 0);
        ASSERT_EQ(device.erases, fail ? 0u : 1u);
        ASSERT_EQ(device.resets, fail ? 0u : 1u);
    }
    return 0;
}

static int test_config_load_rejects_invalid_strings_before_open(void)
{
    const char* bad[] = {
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456",  /* 33 > 32 */
        "caf\xc3\xa9",                         /* UTF-8 */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char xml[256];
        snprintf(xml, sizeof(xml),
                 "<XCT><Field><HPD>1</HPD><F_Table_Name>%s</F_Table_Name>"
                 "</Field></XCT>", bad[i]);
        char path[64];
        ASSERT_EQ(make_temp_path(path), 0);
        ASSERT_EQ(write_file(path, xml), 0);
        writable_device_reset();
        char* argv[] = { "config", "load", "--no-crypt", path };
        int rc = run_writable(cmd_config, 4, argv);
        remove(path);
        ASSERT_EQ(rc, 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(device.mutations, 0);
    }
    return 0;
}

#endif

static int test_write_command_writes_strings_and_numbers_in_one_batch(void)
{
    writable_device_reset();
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, device.page, sizeof(expected));
    const alltrax_var_def* name = alltrax_find_var("F_Table_Name");
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    alltrax_encode_string(name, "NEW MAP", expected + (name->address - VARSET_ADDR));
    expected[hpd->address - VARSET_ADDR] = 1;

    char text[] = "F_Table_Name=NEW MAP", number[] = "HPD=1";
    char* argv[] = { "write", text, number, "--field-map" };
    ASSERT_EQ(run_writable(cmd_write, 4, argv), 0);
    ASSERT_EQ(device.erases, 1);
    ASSERT_MEM_EQ(device.page, expected, sizeof(expected));

    /* An empty value clears the field to spaces. */
    char clear[] = "F_Table_Name=";
    char* argv2[] = { "write", clear, "--field-map" };
    ASSERT_EQ(run_writable(cmd_write, 3, argv2), 0);
    alltrax_encode_string(name, "", expected + (name->address - VARSET_ADDR));
    ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
    return 0;
}

/* Give every factory curve distinct valid points and return the user page
 * expected after restoring the selected curve (NULL: all). */
static void prepare_factory_curves(const char* only, uint8_t expected[VARSET_SIZE])
{
    memcpy(expected, device.page, VARSET_SIZE);
    for (size_t c = 0; c < alltrax_curve_count(); c++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(c);
        for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
            uint16_t x = (uint16_t)(c * 100 + i * 7), y = (uint16_t)(c * 100 + i * 9);
            uint8_t* fx = device.factory + (def->factory_x_address - FACTSET_ADDR) + i * 2;
            uint8_t* fy = device.factory + (def->factory_y_address - FACTSET_ADDR) + i * 2;
            fx[0] = (uint8_t)x; fx[1] = (uint8_t)(x >> 8);
            fy[0] = (uint8_t)y; fy[1] = (uint8_t)(y >> 8);
        }
        if (only && strcmp(def->name, only) != 0)
            continue;
        memcpy(expected + (def->x_address - VARSET_ADDR),
               device.factory + (def->factory_x_address - FACTSET_ADDR),
               ALLTRAX_CURVE_POINTS * 2);
        memcpy(expected + (def->y_address - VARSET_ADDR),
               device.factory + (def->factory_y_address - FACTSET_ADDR),
               ALLTRAX_CURVE_POINTS * 2);
    }
}

static int test_curve_reset_restores_in_one_erase(void)
{
    const char* types[] = { "all", "torque" };
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        writable_device_reset();
        uint8_t expected[VARSET_SIZE];
        prepare_factory_curves(strcmp(types[t], "all") ? types[t] : NULL, expected);
        char type[16];
        snprintf(type, sizeof(type), "%s", types[t]);
        char* argv[] = { "curve", "reset", type, "--field-map" };
        ASSERT_EQ(run_writable(cmd_curve, 4, argv), 0);
        ASSERT_EQ(device.erases, 1);
        ASSERT_EQ(device.mode, RUN_MODE_RUN);
        ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
    }
    return 0;
}

static int test_curve_reset_read_failure_writes_nothing(void)
{
    /* curve reset all reads 2 factory arrays for each of the 5 curves, then
     * the current field map name. */
    for (unsigned n = 1; n <= 11; n++) {
        writable_device_reset();
        uint8_t expected[VARSET_SIZE];
        prepare_factory_curves(NULL, expected);
        uint8_t original[VARSET_SIZE];
        memcpy(original, device.page, sizeof(original));
        device.fail_read_at = n;
        char* argv[] = { "curve", "reset", "all", "--field-map" };
        ASSERT_EQ(run_writable(cmd_curve, 4, argv), 1);
        ASSERT_EQ(device.reads, n);
        ASSERT_EQ(device.mutations, 0);
        ASSERT_MEM_EQ(device.page, original, sizeof(original));
    }
    char* bogus[] = { "curve", "reset", "bogus" };
    ASSERT_EQ(run_writable(cmd_curve, 3, bogus), 1);
    ASSERT_EQ(open_calls, 0);
    return 0;
}

/* Run one of the --reset command lines below against the writable device. */
static int run_reset_case(int which)
{
    char hpd[] = "HPD=1";
    char* write_before[] = { "write", "--reset", hpd };
    char* write_after[] = { "write", hpd, "--reset" };
    char* curve_set[] = { "curve", "set", "linearization", "--preset", "linear", "--reset" };
    char* curve_reset[] = { "curve", "reset", "all", "--reset", "--field-map" };
    switch (which) {
    case 0: return run_writable(cmd_write, 3, write_before);
    case 1: return run_writable(cmd_write, 3, write_after);
    case 2: return run_writable(cmd_curve, 6, curve_set);
    default: return run_writable(cmd_curve, 5, curve_reset);
    }
}

static int test_reset_flag_sends_one_reset_after_success(void)
{
    for (int which = 0; which < 4; which++) {
        writable_device_reset();
        ASSERT_EQ(run_reset_case(which), 0);
        ASSERT_EQ(device.erases, 1);
        ASSERT_EQ(device.resets, 1);
        ASSERT_EQ(device.mode, RUN_MODE_RUN);  /* RUN restored before reset */
    }
    return 0;
}

static int test_reset_flag_sends_no_reset_after_failure(void)
{
    for (int which = 0; which < 4; which++) {
        /* Invalid factory settings fail writes; a failed read fails curve reset. */
        writable_device_reset();
        device.fact_goodset = 0xFFFF;
        device.fail_read_at = which == 3 ? 1 : 0;
        ASSERT_EQ(run_reset_case(which), 1);
        ASSERT_EQ(device.erases, 0);
        ASSERT_EQ(device.resets, 0);
    }
    return 0;
}

/* Run `get <name>` against the simulated device with `value` at the
 * variable's address, returning its stdout line in `out`. */
static int run_get(const char* name, uint32_t value, char* out, size_t out_size)
{
    const alltrax_var_def* var = name ? alltrax_find_var(name) : NULL;
    if (name && !var) return -1;
    char arg[64];
    snprintf(arg, sizeof(arg), "%s", name ? name : "");
    char* argv[] = { "get", arg };
    fake_ctrl = (alltrax_controller) {
        .vid = ALLTRAX_VID, .pid = ALLTRAX_PID_XCT,
        .controller_type = ALLTRAX_CONTROLLER_XCT,
        .boot_rev = 5002, .program_rev = 5005,
    };
    memset(&device, 0, sizeof(device));
    device.enabled = true;
    device.value_addr = var ? var->address : 0;
    device.value = value;
    open_succeeds = true;

    char path[] = "/tmp/alltrax-get-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    int rc = cmd_get(name ? 2 : 1, argv);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    open_succeeds = false;
    device.enabled = false;
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, out, out_size - 1);
    close(fd);
    remove(path);
    out[n > 0 ? n : 0] = '\0';
    return rc;
}

static int test_get_prints_full_integer_range(void)
{
    const struct { const char* name; uint32_t raw; const char* text; } cases[] = {
        { "SerialNum",         0,           " 0 " },
        { "SerialNum",         2147483647u, " 2147483647 " },
        { "SerialNum",         2147483648u, " 2147483648 " },
        { "SerialNum",         4294967295u, " 4294967295 " },
        { "N_Speed_Limit_Max", 0xFFFF,      " -1 RPM" },
        { "N_Speed_Limit_Max", 0x8000,      " -32768 RPM" },
        { "N_Speed_Limit",     4000,        " 4000 RPM" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char out[256];
        ASSERT_EQ(run_get(cases[i].name, cases[i].raw, out, sizeof(out)), 0);
        ASSERT_TRUE(strncmp(out, cases[i].name, strlen(cases[i].name)) == 0);
        ASSERT_TRUE(strstr(out, cases[i].text) != NULL);
    }
    return 0;
}

static int test_get_keeps_scaled_formatting(void)
{
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    ASSERT_TRUE(var != NULL);
    ASSERT_TRUE(var->scale != 1.0);
    /* Raw value for 28.0 V in this variable's scale and offset. */
    uint32_t raw = (uint32_t)lround(28.0 / var->scale) + (uint32_t)var->offset;
    char out[256];
    ASSERT_EQ(run_get("LoBat_Vlim", raw, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, " 28.0 ") != NULL);
    return 0;
}

static int test_get_profile_current_shows_amps_and_signed_percent(void)
{
    char out[256];
    ASSERT_EQ(run_get("N_Max_Arm_Motor_Amps", 4095, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "100.0 % (400.0 A)") != NULL);
    ASSERT_EQ(run_get("U2_Max_Arm_Regen_Amps", (uint16_t)-2048, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "-50.0 % (200.0 A)") != NULL);
    return 0;
}

static int test_get_lists_settings_without_factory_copies(void)
{
    static char out[16384];
    ASSERT_EQ(run_get(NULL, 0, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "Speed_Limit_On") != NULL);
    ASSERT_TRUE(strstr(out, "Tach_4_8") != NULL);
    ASSERT_TRUE(strstr(out, "Throttle_Type_Name") != NULL);
    ASSERT_NULL(strstr(out, "F_Speed_Limit_On"));
    return 0;
}

static void set_page_bytes(const char* name, const char* bytes, size_t len)
{
    const alltrax_var_def* var = alltrax_find_var(name);
    memcpy(device.page + (var->address - VARSET_ADDR), bytes, len);
}

static int run_settings_write(const char* a, const char* b, const char* c)
{
    const char* args[] = { a, b, c };
    char text[3][80];
    char* argv[4] = { "write" };
    int argc = 1;
    for (size_t i = 0; i < 3 && args[i]; i++) {
        snprintf(text[i], sizeof(text[i]), "%s", args[i]);
        argv[argc++] = text[i];
    }
    return run_writable(cmd_write, argc, argv);
}

static int test_write_amp_suffix(void)
{
    const struct { const char* arg; bool ok; } cases[] = {
        { "N_Max_Arm_Motor_Amps=400A", true },
        { "N_Max_Arm_Motor_Amps=400.01A", false },
        { "N_Max_Arm_Regen_Amps=-10A", false },
        { "N_Max_Arm_Motor_Amps=250AA", false },
        { "N_Max_Arm_Motor_Amps=nanA", false },
        { "N_Throttle_Rate=50A", false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        writable_device_reset();
        ASSERT_EQ(run_settings_write(cases[i].arg, NULL, NULL), cases[i].ok ? 0 : 1);
        if (!cases[i].ok) ASSERT_EQ(device.mutations, 0);
    }
    writable_device_reset();
    ASSERT_EQ(run_settings_write("N_Max_Arm_Motor_Amps=250A",
                                  "N_Max_Arm_Regen_Amps=200A", NULL), 0);
    const alltrax_var_def* motor = alltrax_find_var("N_Max_Arm_Motor_Amps");
    const alltrax_var_def* regen = alltrax_find_var("N_Max_Arm_Regen_Amps");
    ASSERT_EQ(get_le16(device.page + (motor->address - VARSET_ADDR)), 2559);
    ASSERT_EQ((int16_t)get_le16(device.page + (regen->address - VARSET_ADDR)), -2048);
    return 0;
}

static int test_field_map_writes_require_flag(void)
{
    char turbo[] = "Turbo_Start_RPM=2000", name[] = "F_Table_Name=MAP";
    char point0[] = "0,0", point1[] = "750,50";
    char* write_scalar[] = { "write", turbo };
    char* write_name[] = { "write", name };
    char* set_field[] = { "curve", "set", "field", point0, point1 };
    char* reset_gen[] = { "curve", "reset", "genfield" };
    char* reset_all[] = { "curve", "reset", "all" };
    struct { int (*cmd)(int, char**); int argc; char** argv; } cases[] = {
        { cmd_write, 2, write_scalar }, { cmd_write, 2, write_name },
        { cmd_curve, 5, set_field }, { cmd_curve, 3, reset_gen },
        { cmd_curve, 3, reset_all },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        writable_device_reset();
        ASSERT_EQ(run_writable(cases[i].cmd, cases[i].argc, cases[i].argv), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(device.mutations, 0);
    }
    return 0;
}

/* A field-map write without F_Table_Name re-writes the current name. */
static int test_field_map_writes_include_current_name(void)
{
    writable_device_reset();
    set_page_bytes("F_Table_Name", "AB\0\0", 4);  /* Read back as "AB" */
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, device.page, sizeof(expected));
    const alltrax_var_def* name = alltrax_find_var("F_Table_Name");
    const alltrax_var_def* turbo = alltrax_find_var("Turbo_Start_RPM");
    alltrax_encode_string(name, "AB", expected + (name->address - VARSET_ADDR));
    alltrax_encode_var(turbo, 2000, expected + (turbo->address - VARSET_ADDR));

    char arg[] = "Turbo_Start_RPM=2000";
    char* argv[] = { "write", arg, "--field-map" };
    ASSERT_EQ(run_writable(cmd_write, 3, argv), 0);
    ASSERT_EQ(device.erases, 1);
    ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
    return 0;
}

static int test_field_map_recovery_uses_factory_name(void)
{
    writable_device_reset();
    set_page_bytes("F_Table_Name", "INVALID", 7);
    device.page[0] = device.page[1] = 0xFF;
    const alltrax_var_def* name = alltrax_find_var("F_Table_Name");
    size_t offset = name->address - VARSET_ADDR;
    memcpy(device.factory + offset, "FACTORY", 7);

    char arg[] = "Turbo_Start_RPM=2000";
    char* argv[] = { "write", arg, "--field-map" };
    ASSERT_EQ(run_writable(cmd_write, 3, argv), 0);
    ASSERT_EQ(device.erases, 2);
    uint8_t expected[32];
    ASSERT_EQ(alltrax_encode_string(name, "FACTORY", expected), 32);
    ASSERT_MEM_EQ(device.page + offset, expected, sizeof(expected));
    return 0;
}

#ifdef TEST_CONFIG
/* Load a plain XML file into the writable device. */
static int run_config_load(const char* xml, bool field_map)
{
    char path[64];
    if (make_temp_path(path) || write_file(path, xml)) return -1;
    char* argv[] = { "config", "load", "--no-crypt", path, "--field-map" };
    int rc = run_writable(cmd_config, field_map ? 5 : 4, argv);
    remove(path);
    return rc;
}

static int test_config_load_skips_field_map_without_flag(void)
{
    const char* xml = "<XCT><Node-Throttle><HPD>1</HPD></Node-Throttle>"
                      "<Node-Field><Turbo_Start_RPM>2000</Turbo_Start_RPM>"
                      "</Node-Field></XCT>";
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    const alltrax_var_def* turbo = alltrax_find_var("Turbo_Start_RPM");
    for (int field_map = 0; field_map <= 1; field_map++) {
        writable_device_reset();
        uint8_t expected[VARSET_SIZE];
        memcpy(expected, device.page, sizeof(expected));
        expected[hpd->address - VARSET_ADDR] = 1;
        if (field_map)
            alltrax_encode_var(turbo, 2000, expected + (turbo->address - VARSET_ADDR));
        ASSERT_EQ(run_config_load(xml, field_map), 0);
        ASSERT_EQ(device.erases, 1);
        ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
    }
    /* A file with only field-map data has nothing to write without the flag. */
    writable_device_reset();
    ASSERT_EQ(run_config_load("<XCT><Node-Field><Turbo_Start_RPM>2000"
                              "</Turbo_Start_RPM></Node-Field></XCT>", false), 1);
    ASSERT_EQ(open_calls, 0);
    return 0;
}

static int test_config_load_fails_closed_on_bad_values(void)
{
    const char* bad[] = {
        "<HPD>2</HPD>", "<HPD>yes</HPD>", "<HPD></HPD>",
        "<N_Speed_Limit>12x</N_Speed_Limit>", "<N_Speed_Limit>4000,</N_Speed_Limit>",
        "<N_Speed_Limit>4000.5</N_Speed_Limit>",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char xml[256];
        snprintf(xml, sizeof(xml), "<XCT><Node-Throttle>%s<Speed_Limit_On>1"
                 "</Speed_Limit_On></Node-Throttle></XCT>", bad[i]);
        writable_device_reset();
        ASSERT_EQ(run_config_load(xml, false), 1);
        ASSERT_EQ(open_calls, 0);
    }
    writable_device_reset();
    ASSERT_EQ(run_config_load("<XCT><Node-Throttle><HPD> 1 </HPD></Node-Throttle></XCT>",
                              false), 0);
    ASSERT_EQ(device.page[alltrax_find_var("HPD")->address - VARSET_ADDR], 1);
    return 0;
}

static int test_config_save_writes_only_settings_in_toolkit_nodes(void)
{
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    memset(&device, 0, sizeof(device));
    ASSERT_EQ(run_config_save(path, 5005), 0);
    char* xml = read_file(path);
    remove(path);
    ASSERT_TRUE(xml != NULL);
    const char* absent[] = {
        "<F_F_Table_Name>", "<F_Reverse_Field_Weaken_Percent>",
        "<F_Speed_Limit_On>", "<Relay_Off_At_Zero>", "<RunMode>", "<Node-Tach/>",
    };
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++)
        ASSERT_NULL(strstr(xml, absent[i]));
    const char* tach = strstr(xml, "<Node-Tach>");
    const char* other = strstr(xml, "<Node-Other_Settings>");
    const char* throttle = strstr(xml, "<Node-Throttle>");
    const char* field = strstr(xml, "<Node-Field>");
    ASSERT_TRUE(tach && other && throttle && field);
    const char* speed = strstr(xml, "<Speed_Limit_On>");
    const char* name = strstr(xml, "<Throttle_Type_Name>");
    ASSERT_TRUE(speed > tach && speed < other);
    ASSERT_TRUE(name > throttle && name < field);
    ASSERT_TRUE(strstr(xml, "<Throttle_Type_Name>                </Throttle_Type_Name>") != NULL);
    free(xml);
    return 0;
}

#endif

int main(void)
{
    printf("CLI mutation preflight (no USB):\n");
    RUN_TEST(test_valid_strings_reach_open);
    RUN_TEST(test_invalid_strings_rejected_before_open);
    RUN_TEST(test_numeric_and_bool_assignments_reach_open);
    RUN_TEST(test_read_only_assignments_use_library_diagnostics);
    RUN_TEST(test_invalid_numeric_text_rejected_before_open);
    RUN_TEST(test_throttle_enum_assignments_use_shared_validation);
    RUN_TEST(test_invalid_curve_arguments_rejected_before_open);
    RUN_TEST(test_curve_csv_validation);
    RUN_TEST(test_curve_presets_reach_open);
    RUN_TEST(test_write_command_writes_strings_and_numbers_in_one_batch);
    RUN_TEST(test_curve_reset_restores_in_one_erase);
    RUN_TEST(test_curve_reset_read_failure_writes_nothing);
    RUN_TEST(test_reset_flag_sends_one_reset_after_success);
    RUN_TEST(test_reset_flag_sends_no_reset_after_failure);
    RUN_TEST(test_get_prints_full_integer_range);
    RUN_TEST(test_get_keeps_scaled_formatting);
    RUN_TEST(test_get_profile_current_shows_amps_and_signed_percent);
    RUN_TEST(test_get_lists_settings_without_factory_copies);
    RUN_TEST(test_field_map_writes_require_flag);
    RUN_TEST(test_field_map_writes_include_current_name);
    RUN_TEST(test_field_map_recovery_uses_factory_name);
    RUN_TEST(test_write_amp_suffix);
    RUN_TEST(test_options_accepted_before_and_after_positionals);
    RUN_TEST(test_invalid_options_and_argument_counts_rejected);
    RUN_TEST(test_write_options_map_to_library_options);
    RUN_TEST(test_bad_command_options_rejected_before_open);
    RUN_TEST(test_fw_flag_reaches_write_options_in_any_position);
#ifdef TEST_CONFIG
    RUN_TEST(test_config_curves_rejected_before_any_settings_write);
    RUN_TEST(test_valid_curve_config_reaches_open);
    RUN_TEST(test_invalid_throttle_config_rejected_before_transport);
    RUN_TEST(test_config_save_includes_every_group_and_curve);
    RUN_TEST(test_config_save_any_read_failure_preserves_destination);
    RUN_TEST(test_config_save_rejects_invalid_settings);
    RUN_TEST(test_config_save_on_unreadable_firmware_preserves_destination);
    RUN_TEST(test_config_save_encrypts_before_writing);
    RUN_TEST(test_config_save_write_failure_removes_partial_file);
    RUN_TEST(test_config_save_write_failure_keeps_device_paths);
    RUN_TEST(test_config_round_trip_restores_strings_in_one_erase);
    RUN_TEST(test_config_load_rejects_invalid_strings_before_open);
    RUN_TEST(test_config_load_reset_flag_sends_one_reset);
    RUN_TEST(test_config_load_skips_field_map_without_flag);
    RUN_TEST(test_config_load_fails_closed_on_bad_values);
    RUN_TEST(test_config_save_writes_only_settings_in_toolkit_nodes);
#endif
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
