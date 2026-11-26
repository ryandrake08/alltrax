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
#include <time.h>
#include <unistd.h>
#ifdef ALLTRAX_HAVE_CRYPTO
#include <openssl/evp.h>
#endif

int test_pass_count;
int test_fail_count;
static unsigned open_calls, unexpected_calls;
static bool open_succeeds;
static alltrax_controller fake_ctrl;
static unsigned error_calls, close_calls, transport_calls;
static unsigned reconnect_waits, reconnect_timeout;
static bool reconnect_succeeds;
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

alltrax_error alltrax_wait_for_reconnect(alltrax_controller* ctrl,
    unsigned timeout_ms)
{
    if (ctrl != &fake_ctrl || !ctrl->reset_sent ||
        strcmp(ctrl->reset_model, "XCT48400-DCS") != 0)
        unexpected_calls++;
    reconnect_waits++;
    reconnect_timeout = timeout_ms;
    return reconnect_succeeds ? ALLTRAX_OK : ALLTRAX_ERR_TIMEOUT;
}

/* Config export references this transport-owned formatter; these preflight
 * tests never export or invoke it. */
const char* alltrax_strerror(alltrax_error err)
{
    (void)err;
    unexpected_calls++;
    return "unexpected error formatting";
}

#define WIRE_LOG_MAX 256

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
    unsigned backup_clears, fan_triggers;
    bool keyswitch;
    uint16_t var_goodset, fact_goodset;
    uint32_t value_addr, value;
    uint8_t page[VARSET_SIZE];
    uint8_t factory[FLASH_PAGE_SIZE];
    uint8_t history_page[DATA_SIZE];
    uint8_t mode;
    const char* model;
    uint32_t throttle_mask;
    uint16_t rated_volts;  /* 0: 48 V */
    bool filter_throttles;
    uint32_t monitor_stop_addr;
    unsigned samples_left;
    uint8_t monitor_ram[0x166];
    bool monitor_speed_sensor, monitor_bms;
    uint8_t wire[WIRE_LOG_MAX][PACKET_SIZE];  /* Every request, in order */
    size_t wire_count;
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
    if (device.wire_count < WIRE_LOG_MAX)
        memcpy(device.wire[device.wire_count], data, PACKET_SIZE);
    device.wire_count++;
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
    else if (addr == ADDR_BACKUP_RAM && data[1] == BACKUP_RAM_SIZE &&
             memcmp(data + 8, (uint8_t[BACKUP_RAM_SIZE]){0}, BACKUP_RAM_SIZE) == 0)
        device.backup_clears++;
    else if (addr == 0x2000F204 && data[1] == 1 && data[8] == 1)
        device.fan_triggers++;
    else if (addr >= VARSET_ADDR && addr - VARSET_ADDR + data[1] <= VARSET_SIZE)
        memcpy(device.page + (addr - VARSET_ADDR), data + 8, data[1]);
    return ALLTRAX_OK;
}

/* A 48V XCT on write-validated firmware with every throttle type allowed. */
static uint8_t identity_byte(uint32_t a)
{
    const char* model = device.model ? device.model : "XCT48400-DCS";
    if (a == ADDR_HARDWARE_CONFIG + 6) return device.monitor_speed_sensor;
    if (device.monitor_stop_addr) {
        if (a == ADDR_HARDWARE_CONFIG + 8) return device.monitor_bms;
        if (a == ADDR_HARDWARE_CONFIG + 10 || a == ADDR_HARDWARE_CONFIG + 21) return 1;
    }
    const struct { uint32_t addr; uint32_t value; } words[] = {
        { ADDR_CONTROLLER_INFO + 0x24, 5002 }, { ADDR_CONTROLLER_INFO + 0x28, 5005 },
        { ADDR_HARDWARE_CONFIG, (device.rated_volts ? device.rated_volts : 48u) | (400u << 16) },
        { ADDR_HARDWARE_CONFIG + 16, device.filter_throttles ? device.throttle_mask : 0x7FF },
        { ADDR_BOOT_REV, 5002 }, { ADDR_PRGM_REV, fake_ctrl.program_rev },
    };
    if (a >= ADDR_CONTROLLER_INFO && a - ADDR_CONTROLLER_INFO < strlen(model))
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
        /* Successful page-erase reply */
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
    if (device.monitor_stop_addr && addr == device.monitor_stop_addr) {
        if (device.samples_left == 0) {
            raise(SIGINT);
            return ALLTRAX_ERR_USB;
        }
        device.samples_left--;
    }
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
        else if (a >= DATA_ADDR && a - DATA_ADDR < DATA_SIZE)
            buf[8 + i] = device.history_page[a - DATA_ADDR];
        if (device.value_addr && a - device.value_addr < 4)
            buf[8 + i] = (uint8_t)(device.value >> ((a - device.value_addr) * 8));
        if (device.monitor_stop_addr && a - 0x2000F000u < sizeof(device.monitor_ram))
            buf[8 + i] = device.monitor_ram[a - 0x2000F000u];
        if (a == 0x2000F090) buf[8 + i] = device.keyswitch;
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
            char* argv[] = { "write", arg };
            open_calls = unexpected_calls = 0;
            ASSERT_EQ(cmd_write(2, argv), 1); /* The fake open always fails. */
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
#ifdef ALLTRAX_HAVE_XML
        char* argv[] = { "config", "load", "--key", "none", path, "--throttle", "--field-map" };
        rc = cmd_config(7, argv);
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

#ifdef ALLTRAX_HAVE_XML
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
    const cli_option_mask allowed = CLI_OPT_SKIP_RULES | CLI_OPT_ALLOW_UNVALIDATED |
                                    CLI_OPT_PLOT | CLI_OPT_FILE;
    char* argv[] = {
        "set", "--plot", "linearization", "--skip-rules", "-1,0",
        "--file", "lin.csv", "-", "--allow-unvalidated-firmware",
    };
    cli_flags flags;
    ASSERT_EQ(cli_parse_args(9, argv, "curve set", allowed, 1, -1, &flags), 3);
    ASSERT_STR_EQ(argv[1], "linearization");
    ASSERT_STR_EQ(argv[2], "-1,0");
    ASSERT_STR_EQ(argv[3], "-");
    ASSERT_TRUE(flags.plot);
    ASSERT_TRUE(flags.skip_rules);
    ASSERT_TRUE(flags.allow_unvalidated_firmware);
    ASSERT_FALSE(flags.factory);
    ASSERT_STR_EQ(flags.file, "lin.csv");
    ASSERT_NULL(flags.preset);
    return 0;
}

static int test_invalid_options_and_argument_counts_rejected(void)
{
    const cli_option_mask allowed = CLI_OPT_PLOT | CLI_OPT_FILE;
    struct { int argc; char* argv[5]; int min, max; } bad[] = {
        { 2, { "cmd", "--bogus" }, 0, -1 },            /* unknown */
        { 2, { "cmd", "--skip-rules" }, 0, -1 },   /* not accepted */
        { 2, { "cmd", "--file" }, 0, -1 },             /* missing value */
        { 3, { "cmd", "--file", "--plot" }, 0, -1 },   /* option as value */
        { 5, { "cmd", "--file", "a", "--file", "b" }, 0, -1 }, /* repeated */
        { 3, { "cmd", "one", "two" }, 1, 1 },          /* extra */
        { 2, { "cmd", "--plot" }, 1, 1 },              /* missing */
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
    char* argv[] = { "write", "--allow-unvalidated-firmware", "--skip-rules" };
    cli_flags flags;
    ASSERT_EQ(cli_parse_args(3, argv, "write", CLI_OPTS_WRITE | CLI_OPT_SKIP_RULES,
                             0, -1, &flags), 0);
    ASSERT_TRUE(flags.skip_rules);
    alltrax_write_report report;
    alltrax_write_opts opts = cli_write_opts(&flags, &report);
    ASSERT_TRUE(opts.report == &report);
    ASSERT_TRUE(opts.skip_fw_check);
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
        { 4, { "curve", "diff", "linearization", "--plot" } },
        { 3, { "curve", "list", "extra" } },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        open_calls = transport_calls = 0;
        ASSERT_EQ(cmd_curve(bad[i].argc, bad[i].argv), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(transport_calls, 0);
    }
    char* reset_extra[] = { "reset", "extra" };
    char* reset_bogus[] = { "reset", "--plot" };
    char assign[] = "HPD=1";
    char* write_bogus[] = { "write", assign, "--plot" };
    open_calls = 0;
    ASSERT_EQ(cmd_reset(2, reset_extra), 1);
    ASSERT_EQ(cmd_reset(2, reset_bogus), 1);
    ASSERT_EQ(cmd_write(3, write_bogus), 1);
    ASSERT_EQ(open_calls, 0);
#ifdef ALLTRAX_HAVE_XML
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

#ifdef ALLTRAX_HAVE_XML
/* Keys used only by these tests. */
#define TEST_KEY_A "0123456789abcdef0123456789ABCDEF0123456789abcdef:0011223344556677"
#define TEST_KEY_B "fedcba9876543210fedcba9876543210fedcba9876543210:8899aabbccddeeff"

/* Run `config save <path> [--key <key>]` against the simulated device;
 * a NULL key omits the option. */
static int run_config_save_as(const char* path, uint32_t program_rev,
    const char* key)
{
    char file[256], key_text[128];
    snprintf(file, sizeof(file), "%s", path);
    snprintf(key_text, sizeof(key_text), "%s", key ? key : "");
    char* argv[] = { "config", "save", file, "--key", key_text };
    fake_ctrl = (alltrax_controller) {
        .vid = ALLTRAX_VID, .pid = ALLTRAX_PID_XCT,
        .controller_type = ALLTRAX_CONTROLLER_XCT,
        .boot_rev = 5002, .program_rev = program_rev,
    };
    open_calls = error_calls = close_calls = transport_calls = 0;
    device.enabled = true;
    device.reads = device.mutations = 0;
    open_succeeds = true;
    int rc = cmd_config(key ? 5 : 3, argv);
    open_succeeds = false;
    device.enabled = false;
    return rc;
}

static int run_config_save(const char* path, uint32_t program_rev)
{
    return run_config_save_as(path, program_rev, "none");
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

#ifdef ALLTRAX_HAVE_CRYPTO
static int test_config_save_encrypts_before_writing(void)
{
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    memset(&device, 0, sizeof(device));
    ASSERT_EQ(run_config_save_as(path, 5005, TEST_KEY_A), 0);
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

/* True if the file decrypts with `key_text` to a config document. */
static bool saved_with(const char* path, const char* key_text)
{
    cli_key key;
    if (cli_resolve_key("test", key_text, "ALLTRAX_TEST_UNSET", NULL, &key))
        return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    static unsigned char data[65536];
    size_t n = fread(data, 1, sizeof(data), f);
    fclose(f);
    static char plain[65536 + 8];
    int len = 0, total = 0;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_des_ede3_cbc(), NULL, key.key, key.iv) &&
              EVP_DecryptUpdate(ctx, (unsigned char*)plain, &len, data, (int)n);
    total = len;
    ok = ok && EVP_DecryptFinal_ex(ctx, (unsigned char*)plain + total, &len);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    plain[total + len] = '\0';
    return strstr(plain, "<XCT") != NULL;
}

/* An explicit key wins over ALLTRAX_CONFIG_KEY, which wins over the built-in
 * key; without either, a build with no key fails before opening the device. */
static int test_config_key_precedence(void)
{
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    memset(&device, 0, sizeof(device));
    setenv("ALLTRAX_CONFIG_KEY", TEST_KEY_B, 1);
    ASSERT_EQ(run_config_save_as(path, 5005, TEST_KEY_A), 0);
    ASSERT_TRUE(saved_with(path, TEST_KEY_A));
    ASSERT_EQ(run_config_save_as(path, 5005, NULL), 0);
    ASSERT_TRUE(saved_with(path, TEST_KEY_B));
    ASSERT_EQ(run_config_save_as(path, 5005, "auto"), 0);
    ASSERT_TRUE(saved_with(path, TEST_KEY_B));
    ASSERT_EQ(run_config_save_as(path, 5005, "none"), 0);
    char* plain = read_file(path);
    ASSERT_TRUE(plain != NULL && strstr(plain, "<XCT") != NULL);
    free(plain);

    /* An empty variable counts as unset. */
    setenv("ALLTRAX_CONFIG_KEY", "", 1);
    int rc = run_config_save_as(path, 5005, NULL);
    unsetenv("ALLTRAX_CONFIG_KEY");
#ifdef ALLTRAX_EMBEDDED_KEYS
    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(saved_with(path, TEST_KEY_A) || saved_with(path, TEST_KEY_B));
    struct stat st;
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_size % 8, 0);
    plain = read_file(path);
    ASSERT_TRUE(plain != NULL && strstr(plain, "<XCT") == NULL);
    free(plain);
#else
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(open_calls, 0);
#endif
    remove(path);
    return 0;
}

/* Config files ignore the field-map key's environment variable. */
static int test_file_keys_use_separate_environment_variables(void)
{
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    memset(&device, 0, sizeof(device));
    unsetenv("ALLTRAX_CONFIG_KEY");
    setenv("ALLTRAX_FIELDMAP_KEY", TEST_KEY_A, 1);
    int rc = run_config_save_as(path, 5005, NULL);
    unsetenv("ALLTRAX_FIELDMAP_KEY");
#ifdef ALLTRAX_EMBEDDED_KEYS
    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(saved_with(path, TEST_KEY_A));
#else
    ASSERT_EQ(rc, 1);
#endif
    remove(path);
    return 0;
}

static int test_config_key_rejects_bad_values_before_open(void)
{
    const char* bad[] = {
        "AUTO", "None", "",
        "0123456789abcdef0123456789ABCDEF0123456789abcde:0011223344556677",   /* 47 */
        "0123456789abcdef0123456789ABCDEF0123456789abcdef:001122334455667",   /* 15 */
        TEST_KEY_A "0",
        "0123456789abcdef0123456789ABCDEF0123456789abcdef0011223344556677",   /* no ':' */
        "0123456789abcdef0123456789ABCDEF0123456789abcdeg:0011223344556677",  /* 'g' */
        "0123456789abcdef0123456789ABCDEF0123456789abcdef:00112233445566 7",
    };
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    remove(path);
    unsetenv("ALLTRAX_CONFIG_KEY");
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT_EQ(run_config_save_as(path, 5005, bad[i]), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(access(path, F_OK), -1);
    }

    /* The variable holds a key only; an explicit option ignores it. */
    const char* bad_env[] = { "auto", "none", "0123" };
    for (size_t i = 0; i < sizeof(bad_env) / sizeof(bad_env[0]); i++) {
        setenv("ALLTRAX_CONFIG_KEY", bad_env[i], 1);
        ASSERT_EQ(run_config_save_as(path, 5005, NULL), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(run_config_save_as(path, 5005, "auto"), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(access(path, F_OK), -1);
        char* load[] = { "config", "load", path };
        open_calls = 0;
        ASSERT_EQ(cmd_config(3, load), 1);
        ASSERT_EQ(open_calls, 0);
    }
    ASSERT_EQ(run_config_save_as(path, 5005, TEST_KEY_A), 0);
    unsetenv("ALLTRAX_CONFIG_KEY");
    remove(path);

    char* repeated[] = { "config", "save", path, "--key", "none", "--key", "none" };
    char* missing[] = { "config", "save", path, "--key" };
    open_calls = 0;
    ASSERT_EQ(cmd_config(7, repeated), 1);
    ASSERT_EQ(cmd_config(4, missing), 1);
    ASSERT_EQ(open_calls, 0);
    ASSERT_EQ(access(path, F_OK), -1);
    return 0;
}

#else
/* Without libcrypto, config files are plain XML and the key option must say so. */
static int test_config_requires_plain_xml_without_crypto(void)
{
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    remove(path);
    memset(&device, 0, sizeof(device));
    setenv("ALLTRAX_CONFIG_KEY", TEST_KEY_B, 1);
    const char* refused[] = { NULL, "auto", TEST_KEY_A };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        ASSERT_EQ(run_config_save_as(path, 5005, refused[i]), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(access(path, F_OK), -1);
    }
    unsetenv("ALLTRAX_CONFIG_KEY");
    ASSERT_EQ(run_config_save_as(path, 5005, "none"), 0);
    char* plain = read_file(path);
    ASSERT_TRUE(plain != NULL && strstr(plain, "<XCT") != NULL);
    free(plain);
    char* load[] = { "config", "load", path };
    open_calls = 0;
    ASSERT_EQ(cmd_config(3, load), 1);
    ASSERT_EQ(open_calls, 0);
    remove(path);
    return 0;
}
#endif

/* Run the save with a file size limit (bytes). */
static int run_config_save_size_limited(const char* path, const char* key,
    rlim_t limit)
{
    struct rlimit saved;
    if (getrlimit(RLIMIT_FSIZE, &saved) != 0) return -1;
    struct rlimit limited = saved;
    limited.rlim_cur = limit;
    void (*old_handler)(int) = signal(SIGXFSZ, SIG_IGN);
    if (setrlimit(RLIMIT_FSIZE, &limited) != 0) return -1;
    int rc = run_config_save_as(path, 5005, key);
    setrlimit(RLIMIT_FSIZE, &saved);
    signal(SIGXFSZ, old_handler);
    return rc;
}

/* A 64-byte limit fails the first write. A limit one byte short of the full
 * file lets fwrite() succeed and fails only the final flush in fclose(). */
static int test_config_save_write_failure_removes_partial_file(void)
{
    const char* keys[] = {
        "none",
#ifdef ALLTRAX_HAVE_CRYPTO
        TEST_KEY_A,
#endif
    };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        char path[64];
        ASSERT_EQ(make_temp_path(path), 0);
        memset(&device, 0, sizeof(device));
        ASSERT_EQ(run_config_save_as(path, 5005, keys[k]), 0);
        struct stat st;
        ASSERT_EQ(stat(path, &st), 0);
        const rlim_t limits[] = { 64, (rlim_t)st.st_size - 1 };
        for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
            ASSERT_EQ(write_file(path, "previous backup"), 0);
            ASSERT_EQ(run_config_save_size_limited(path, keys[k],
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
    /* Curves that satisfy the value rules: a straight line over full scale. */
    for (size_t c = 0; c < alltrax_curve_count(); c++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(c);
        const int16_t xs[] = { 0, def->x_raw_max }, ys[] = { 0, def->y_raw_max };
        for (size_t i = 0; i < 2; i++) {
            uint8_t* x = device.page + (def->x_address - VARSET_ADDR) + i * 2;
            uint8_t* y = device.page + (def->y_address - VARSET_ADDR) + i * 2;
            x[0] = (uint8_t)xs[i]; x[1] = (uint8_t)(xs[i] >> 8);
            y[0] = (uint8_t)ys[i]; y[1] = (uint8_t)(ys[i] >> 8);
        }
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

#ifdef ALLTRAX_HAVE_XML
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
        char* argv[] = { "config", "load", "--key", "none", path, "--throttle", "--field-map" };
        int rc = run_writable(cmd_config, 7, argv);
        remove(path);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(device.erases, 1);
        ASSERT_EQ(device.mode, RUN_MODE_RUN);
        ASSERT_MEM_EQ(device.page, original, sizeof(original));
    }
    return 0;
}

static int test_config_load_sends_one_reset_after_success(void)
{
    for (int fail = 0; fail <= 1; fail++) {
        writable_device_reset();
        char path[64];
        ASSERT_EQ(make_temp_path(path), 0);
        ASSERT_EQ(run_config_save(path, 5005), 0);
        if (fail)
            device.fact_goodset = 0xFFFF;
        device.erases = device.resets = 0;
        char* argv[] = { "config", "load", "--key", "none", path, "--throttle", "--field-map" };
        int rc = run_writable(cmd_config, 7, argv);
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
        char* argv[] = { "config", "load", "--key", "none", path };
        int rc = run_writable(cmd_config, 5, argv);
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
    char* argv[] = { "write", text, number };
    ASSERT_EQ(run_writable(cmd_write, 3, argv), 0);
    ASSERT_EQ(device.erases, 1);
    ASSERT_MEM_EQ(device.page, expected, sizeof(expected));

    /* An empty value clears the field to spaces. */
    char clear[] = "F_Table_Name=";
    char* argv2[] = { "write", clear };
    ASSERT_EQ(run_writable(cmd_write, 2, argv2), 0);
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
        char* argv[] = { "curve", "reset", type, "--throttle", "--field-map" };
        ASSERT_EQ(run_writable(cmd_curve, 5, argv), 0);
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
        char* argv[] = { "curve", "reset", "all", "--throttle", "--field-map" };
        ASSERT_EQ(run_writable(cmd_curve, 5, argv), 1);
        ASSERT_EQ(device.reads, n);
        ASSERT_EQ(device.mutations, 0);
        ASSERT_MEM_EQ(device.page, original, sizeof(original));
    }
    char* bogus[] = { "curve", "reset", "bogus" };
    ASSERT_EQ(run_writable(cmd_curve, 3, bogus), 1);
    ASSERT_EQ(open_calls, 0);
    return 0;
}

/* Run one of the settings writes below against the writable device. */
static int run_reset_case(int which)
{
    char hpd[] = "HPD=1", low[] = "LoBat_Vlim=20.5";
    char* write_hpd[] = { "write", hpd };
    char* write_low[] = { "write", low };
    char* curve_set[] = { "curve", "set", "speed", "--preset", "standard" };
    char* curve_reset[] = { "curve", "reset", "all", "--throttle", "--field-map" };
    switch (which) {
    case 0: return run_writable(cmd_write, 2, write_hpd);
    case 1: return run_writable(cmd_write, 2, write_low);
    case 2: return run_writable(cmd_curve, 5, curve_set);
    default: return run_writable(cmd_curve, 5, curve_reset);
    }
}

static int test_settings_writes_send_one_reset_after_success(void)
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

static int test_settings_writes_send_no_reset_after_failure(void)
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

static int test_linearization_and_field_map_curves_do_not_reset(void)
{
    char* lin[] = { "curve", "set", "linearization", "--preset", "linear" };
    char a[] = "0,10", b[] = "375,5", c[] = "750,1";
    char* field[] = { "curve", "set", "field", a, b, c };
    writable_device_reset();
    ASSERT_EQ(run_writable(cmd_curve, 5, lin), 0);
    ASSERT_EQ(device.erases, 1);
    ASSERT_EQ(device.resets, 0);
    writable_device_reset();
    ASSERT_EQ(run_writable(cmd_curve, 6, field), 0);
    ASSERT_EQ(device.erases, 1);
    ASSERT_EQ(device.resets, 0);
    return 0;
}

static int run_cli_output(int (*cmd)(int, char**), int argc, char** argv,
    char* out, size_t out_size)
{
    FILE* file = tmpfile();
    if (!file) return -1;
    fflush(NULL);
    int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);
    int rc = -1;
    if (saved_out >= 0 && saved_err >= 0 &&
        dup2(fileno(file), STDOUT_FILENO) >= 0 && dup2(fileno(file), STDERR_FILENO) >= 0)
        rc = run_writable(cmd, argc, argv);
    fflush(NULL);
    if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
    if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
    rewind(file);
    size_t n = fread(out, 1, out_size - 1, file);
    out[n] = '\0';
    fclose(file);
    return rc;
}

static int get_output(int argc, char** argv, char* out, size_t out_size)
{
    return run_cli_output(cmd_get, argc, argv, out, out_size);
}

/* Set the raw value of a named variable and print its get output. */
static int run_get(const char* name, uint32_t value, char* out, size_t out_size)
{
    const alltrax_var_def* var = name ? alltrax_find_var(name) : NULL;
    if (name && !var) return -1;
    char arg[64];
    snprintf(arg, sizeof(arg), "%s", name ? name : "");
    char* argv[] = { "get", arg };
    memset(&device, 0, sizeof(device));
    device.value_addr = var ? var->address : 0;
    device.value = value;
    return get_output(name ? 2 : 1, argv, out, out_size);
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

static int test_write_reports_field_map_date_stamp(void)
{
    static char out[4096];
    char name[] = "F_Table_Name=SAMPLE-MAP-1";
    char* field_map[] = { "write", name };
    writable_device_reset();
    ASSERT_EQ(run_cli_output(cmd_write, 2, field_map, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "Stamped field-map date\n") != NULL);

    char voltage[] = "LoBat_Vlim=28.0";
    char* setting[] = { "write", voltage };
    writable_device_reset();
    ASSERT_EQ(run_cli_output(cmd_write, 2, setting, out, sizeof(out)), 0);
    ASSERT_NULL(strstr(out, "Stamped field-map date"));
    return 0;
}

static int test_debug_read_rejects_bad_arguments_before_open(void)
{
    char* cases[][5] = {
        { "debug-read" },
        { "debug-read", "0x40013804" },       /* Peripheral register */
        { "debug-read", "0x2000FFFE", "4" },  /* Leaves RAM */
        { "debug-read", "0x20000000", "0" },
        { "debug-read", "0x20000000", "2049" },
        { "debug-read", "0x20000000", "4x" },
        { "debug-read", "zz" },
        { "debug-read", "-1" },
        { "debug-read", "0x120000000" },
        { "debug-read", "0x20000000", "4", "--watch", "50" },
        { "debug-read", "0x20000000", "4", "--watch", "fast" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int argc = 0;
        while (argc < 5 && cases[i][argc]) argc++;
        writable_device_reset();
        ASSERT_EQ(run_writable(cmd_debug_read, argc, cases[i]), 1);
        ASSERT_EQ(open_calls, 0);
    }
    return 0;
}

static int test_debug_read_prints_bytes_and_value(void)
{
    static char out[4096];
    char* word[] = { "debug-read", "0x20000100" };
    writable_device_reset();
    device.value_addr = 0x20000100;
    device.value = 0x12345678;
    ASSERT_EQ(run_cli_output(cmd_debug_read, 2, word, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "0x20000100  78 56 34 12  (u32 305419896)\n");

    char* backup[] = { "debug-read", "0x40006C00", "20" };
    writable_device_reset();
    ASSERT_EQ(run_cli_output(cmd_debug_read, 3, backup, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out,
        "0x40006C00  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00\n"
        "0x40006C10  00 00 00 00\n");
    return 0;
}

static int test_debug_read_watch_repeats_until_stopped(void)
{
    static char out[4096];
    char* argv[] = { "debug-read", "0x20000100", "2", "--watch", "100" };
    writable_device_reset();
    device.monitor_stop_addr = 0x20000100;
    device.samples_left = 2;
    ASSERT_EQ(run_cli_output(cmd_debug_read, 5, argv, out, sizeof(out)), 0);
    size_t samples = 0;
    for (const char* p = out; (p = strstr(p, "0x20000100  00 00  (u16 0)\n")); p++)
        samples++;
    ASSERT_EQ(samples, 2);
    ASSERT_TRUE(strstr(out, " s  0x20000100") != NULL);
    ASSERT_TRUE(strstr(out, "Stopped.\n") != NULL);
    ASSERT_EQ(device.mutations, 0);
    return 0;
}

static int test_get_rejects_unknown_names_before_open(void)
{
    char* argv[] = { "get", "LoBat_Vlim", "NoSuchVar", "HiBat_Vlim" };
    writable_device_reset();
    ASSERT_EQ(run_writable(cmd_get, 4, argv), 1);
    ASSERT_EQ(open_calls, 0);
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

static int test_get_reports_goodset_and_reads_factory(void)
{
    char out[256];
    memset(&device, 0, sizeof(device));
    device.var_goodset = 0xFFFF;
    const alltrax_var_def* limit = alltrax_find_var("LoBat_Vlim");
    device.value_addr = limit->address;
    device.value = 200;
    ASSERT_EQ(alltrax_encode_var(limit, 28.0, device.factory + (limit->address - VARSET_ADDR)), 2);
    char* normal[] = { "get", "LoBat_Vlim" };
    ASSERT_EQ(get_output(2, normal, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "user settings are invalid") && strstr(out, "20.0 V"));
    char* factory[] = { "get", "LoBat_Vlim", "--factory" };
    ASSERT_EQ(get_output(3, factory, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "28.0 V") != NULL);
    ASSERT_NULL(strstr(out, "Warning"));
    device.fact_goodset = 0xFFFF;
    char* invalid[] = { "get", "--factory", "LoBat_Vlim" };
    ASSERT_EQ(get_output(3, invalid, out, sizeof(out)), 1);
    ASSERT_TRUE(strstr(out, "factory settings are invalid") != NULL);
    ASSERT_EQ(get_output(2, normal, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "user and factory settings are invalid") != NULL);
    char* runtime[] = { "get", "--factory", "Fan_Status" };
    ASSERT_EQ(get_output(3, runtime, out, sizeof(out)), 1);
    ASSERT_EQ(reported_error, ALLTRAX_ERR_INVALID_ARG);
    ASSERT_TRUE(strstr(reported_detail, "Fan_Status has no user settings page offset") != NULL);
    ASSERT_EQ(device.mutations, 0);
    return 0;
}

static int test_fan_test_requires_keyswitch_and_writes_once(void)
{
    char* argv[] = { "fan-test" };
    writable_device_reset();
    ASSERT_EQ(run_writable(cmd_fan_test, 1, argv), 1);
    ASSERT_EQ(device.reads, 1);
    ASSERT_EQ(device.mutations, 0);
    device.keyswitch = true;
    ASSERT_EQ(run_writable(cmd_fan_test, 1, argv), 0);
    ASSERT_EQ(device.fan_triggers, 1);
    ASSERT_EQ(device.mutations, 3); /* CAL, Fan_On, RUN */
    ASSERT_EQ(device.mode, RUN_MODE_RUN);
    ASSERT_EQ(device.resets, 0);
    return 0;
}

static int test_clear_autocal_sends_one_reset_after_clear(void)
{
    char* argv[] = { "clear-autocal" };
    writable_device_reset();
    ASSERT_EQ(run_writable(cmd_clear_autocal, 1, argv), 0);
    ASSERT_EQ(device.backup_clears, 1);
    ASSERT_EQ(device.resets, 1);
    ASSERT_EQ(device.mutations, 2);
    writable_device_reset();
    device.fail_read_at = 1; /* Backup write acknowledgement fails. */
    ASSERT_EQ(run_writable(cmd_clear_autocal, 1, argv), 1);
    ASSERT_EQ(device.backup_clears, 1);
    ASSERT_EQ(device.resets, 0);
    return 0;
}

static int test_reset_wait_timeout_does_not_resend_reset(void)
{
    char* argv[] = { "reset" };
    writable_device_reset();
    reconnect_waits = 0;
    reconnect_succeeds = false;
    ASSERT_EQ(run_writable(cmd_reset, 1, argv), 0);
    ASSERT_EQ(reconnect_waits, 1);
    ASSERT_EQ(reconnect_timeout, 7500);
    ASSERT_EQ(device.resets, 1);
    ASSERT_EQ(device.mutations, 1);
    return 0;
}

static void set_page_bytes(const char* name, const char* bytes, size_t len)
{
    const alltrax_var_def* var = alltrax_find_var(name);
    memcpy(device.page + (var->address - VARSET_ADDR), bytes, len);
}

static void monitor_word(size_t offset, uint16_t value)
{
    device.monitor_ram[offset] = (uint8_t)value;
    device.monitor_ram[offset + 1] = (uint8_t)(value >> 8);
}

static void monitor_fixture(unsigned state, bool keyswitch)
{
    writable_device_reset();
    device.monitor_stop_addr = 0x2000F000;
    device.samples_left = 1;
    device.keyswitch = keyswitch;
    device.monitor_bms = true;
    device.monitor_speed_sensor = true;
    device.page[0xA7] = 1;
    device.page[0xBC] = 1;
    device.page[0xBE] = (uint8_t)state;
    set_page_bytes("F_Table_Name", "A,\"B\"", 5);
    monitor_word(0x110, 528);
    monitor_word(0x116, 2047);
    monitor_word(0x118, 2047);
    monitor_word(0x122, 4096);
    monitor_word(0x128, 4095);
    monitor_word(0x138, 3500);
    monitor_word(0x13A, 50);
    monitor_word(0x14A, 875);
    monitor_word(0x15E, (uint16_t)-1);
    monitor_word(0x160, 2);
    monitor_word(0x162, (uint16_t)-1);
    monitor_word(0x164, 75);
    monitor_word(0x096, 65535);
    device.monitor_ram[0x09A] = device.monitor_ram[0x09C] = device.monitor_ram[0x09D] = 1;
    device.monitor_ram[0x09F] = 1;
    device.monitor_ram[0x036] = 1; /* Low Battery previously occurred. */
}

static int test_field_map_history_command(void)
{
    char out[128];
    char* invalid[] = { "field-map", "history", "extra" };
    writable_device_reset();
    ASSERT_EQ(run_cli_output(cmd_field_map, 3, invalid, out, sizeof(out)), 1);
    ASSERT_EQ(open_calls, 0);

    const uint8_t record[] = { 0xEA, 0x07, 24, 11, 'M', 'a', 'p' };
    memset(device.history_page, 0xFF, sizeof(device.history_page));
    memset(device.history_page, ' ', 36);
    memcpy(device.history_page, record, sizeof(record));
    device.history_page[2046] = 1;
    device.history_page[2047] = 0;

    char* argv[] = { "field-map", "history" };
    ASSERT_EQ(run_cli_output(cmd_field_map, 2, argv, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "Map count: 47\n11/24/2026  Map\n");
    ASSERT_EQ(device.mutations, 0);
    return 0;
}

static int test_monitor_display_gates_live_samples(void)
{
    char out[4096];
    char* argv[] = { "monitor", "--rate", "100" };
    for (unsigned state = 0; state < 3; state++) {
        monitor_fixture(state, true);
        if (state == 0) {
            device.page[0xA7] = 0; /* Sensor present, speed limiting off. */
            device.monitor_bms = false;
        }
        if (state == 2) device.page[0xA0] = 10; /* USB uses setpoint. */
        ASSERT_EQ(run_cli_output(cmd_monitor, 3, argv, out, sizeof(out)), 0);
        ASSERT_TRUE(strstr(out, "Motor Voltage         26.4 V") != NULL);
        ASSERT_TRUE(strstr(out, "Bad Vars Code         65535") != NULL);
        ASSERT_TRUE(strstr(out, "Active: Over Temperature") != NULL);
        ASSERT_TRUE(strstr(out, "Previously occurred: Low Battery") != NULL);
        ASSERT_TRUE(strstr(out, "AuxOut                ON") != NULL);
        ASSERT_TRUE(strstr(out, state == 2 ? "Throttle              100 %" : "Throttle              49 %") != NULL);
        if (state == 0) {
            ASSERT_TRUE(strstr(out, "User 1 Input          ON") != NULL);
            ASSERT_TRUE(strstr(out, "Motor Speed           N/A") != NULL);
            ASSERT_NULL(strstr(out, "State of Charge"));
        } else {
            ASSERT_TRUE(strstr(out, "Motor Speed           0 RPM") != NULL);
            ASSERT_TRUE(strstr(out, "State of Charge       87.5 %") != NULL);
            ASSERT_TRUE(strstr(out, "Disconnected") != NULL);
            ASSERT_TRUE(strstr(out, state == 1 ? "Selected Profile      User 2"
                                              : "User2 (Speed)         75 %") != NULL);
        }
        ASSERT_EQ(device.mutations, 0);
    }
    monitor_fixture(1, false);
    ASSERT_EQ(run_cli_output(cmd_monitor, 3, argv, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "Throttle              0 %") != NULL);
    ASSERT_TRUE(strstr(out, "Temperature           -") != NULL);
    ASSERT_TRUE(strstr(out, "User 3 Input          -") != NULL);
    ASSERT_TRUE(strstr(out, "Charger               -") != NULL);
    ASSERT_TRUE(strstr(out, "State of Charge       -") != NULL);
    ASSERT_NULL(strstr(out, "Previously occurred"));
    ASSERT_NULL(strstr(out, "Active:"));

    monitor_fixture(1, false);
    device.monitor_ram[14] = device.monitor_ram[15] = device.monitor_ram[16] = 1;
    ASSERT_EQ(run_cli_output(cmd_monitor, 3, argv, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "Active: Aux Stuck") != NULL);
    ASSERT_TRUE(strstr(out, "Active: Bad Vars") != NULL);
    ASSERT_NULL(strstr(out, "BMS Missing"));
    ASSERT_NULL(strstr(out, "Over Temperature"));
    ASSERT_NULL(strstr(out, "Low Battery"));

    monitor_fixture(1, false);
    device.monitor_ram[15] = 1;
    monitor_word(0x050, 1); /* Bad Vars previously occurred. */
    device.monitor_bms = false;
    ASSERT_EQ(run_cli_output(cmd_monitor, 3, argv, out, sizeof(out)), 0);
    ASSERT_NULL(strstr(out, "Aux Stuck"));
    ASSERT_TRUE(strstr(out, "Previously occurred: Bad Vars") != NULL);

    monitor_fixture(0, true);
    monitor_word(0x096, 0);
    device.page[0] = 0xFF;
    device.factory[0xA0] = 9; /* Factory Pump mode uses the setpoint. */
    device.factory[0xBE] = 1;
    ASSERT_EQ(run_cli_output(cmd_monitor, 3, argv, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "Selected Profile      User 2") != NULL);
    ASSERT_TRUE(strstr(out, "Throttle              100 %") != NULL);
    ASSERT_NULL(strstr(out, "State of Charge")); /* Factory does not expect BMS. */
    ASSERT_NULL(strstr(out, "Bad Vars Code"));
    return 0;
}

static int test_monitor_throttle_reads_three_blocks_and_blanks_with_ksi_off(void)
{
    char out[512];
    char* argv[] = { "monitor", "throttle" };
    for (int on = 0; on <= 1; on++) {
        monitor_fixture(0, on != 0);
        device.monitor_stop_addr = 0x2000F090;
        ASSERT_EQ(run_cli_output(cmd_monitor, 2, argv, out, sizeof(out)), 0);
        ASSERT_EQ(device.reads, 4); /* Three blocks plus interrupted next sample. */
        ASSERT_EQ(device.mutations, 0);
        if (on)
            ASSERT_TRUE(strstr(out, "Input 0 %   Linearized 100 %   Setpoint 100 %   Output 49 %") != NULL);
        else {
            ASSERT_TRUE(strstr(out, "Keyswitch off") != NULL);
            ASSERT_NULL(strchr(out, '%'));
        }
    }
    return 0;
}

static size_t comma_count(const char* line)
{
    size_t count = 0;
    for (; *line && *line != '\n'; line++)
        if (*line == ',') count++;
    return count;
}

static int test_monitor_csv_appends_toolkit_layout_with_timestamp(void)
{
    char path[] = "/tmp/alltrax-monitor-test-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    char out[4096], csv[8192];
    char* argv[] = { "monitor", "--log", path };
    for (int sample = 0; sample < 3; sample++) {
        bool keyswitch = sample < 2;
        monitor_fixture(2, keyswitch);
        device.monitor_ram[14] = sample != 0;
        device.monitor_ram[15] = device.monitor_ram[16] = !keyswitch;
        if (sample == 1) monitor_word(0x096, 0);
        int rc = run_cli_output(cmd_monitor, 3, argv, out, sizeof(out));
        if (rc) remove(path);
        ASSERT_EQ(rc, 0);
        ASSERT_TRUE(strstr(out, sample ? "State of Charge       -" : "State of Charge       87.5 %") != NULL);
    }
    FILE* file = fopen(path, "r");
    remove(path);
    ASSERT_TRUE(file != NULL);
    size_t n = fread(csv, 1, sizeof(csv) - 1, file);
    fclose(file);
    csv[n] = '\0';
    ASSERT_TRUE(strstr(csv, "Model: XCT48400-DCS,,,Serial: 0,,,\"Field Map: A,\"\"B\"\"\"\n") != NULL);
    char* header = strstr(csv, "Date,Time,Keyswitch,Footswitch,Forward Switch,Reverse Switch,");
    ASSERT_TRUE(header != NULL);
    ASSERT_TRUE(strstr(header, "User2 Input Percent (Speed),User1 Input Percent (Regen),") != NULL);
    ASSERT_TRUE(strstr(header, "Bad Vars Code,,,Throttle Position,Throttle Pointer,Overtemp Cap,Timestamp\n") != NULL);
    const char* row = strchr(header, '\n') + 1;
    ASSERT_EQ(comma_count(row), comma_count(header));
    ASSERT_TRUE(strstr(row, ",75,-1,1,49,0,87.5,52.8,") != NULL);
    ASSERT_TRUE(strstr(row, ",65535,,,2047.0,4095.0,3500,") != NULL);
    const char* end = strchr(row, '\n');
    ASSERT_TRUE(end != NULL && end > row && end[-1] == 'Z');
    const char* next_header = strstr(end, "Date,Time,Keyswitch");
    ASSERT_TRUE(next_header != NULL);
    const char* next_row = strchr(next_header, '\n') + 1;
    ASSERT_EQ(comma_count(next_row), comma_count(next_header));
    ASSERT_TRUE(strstr(next_row, ",75,-1,1,49,0,,52.8,") != NULL);
    ASSERT_TRUE(strstr(next_row, ",0,,,2047.0,4095.0,3500,") != NULL);
    const char* off_header = strstr(next_row, "Date,Time,Keyswitch");
    ASSERT_TRUE(off_header != NULL);
    const char* off_row = strchr(off_header, '\n') + 1;
    ASSERT_EQ(comma_count(off_row), comma_count(off_header));
    ASSERT_TRUE(strstr(off_row, ",0,1,1,65535,,,2047.0,4095.0,3500,") != NULL);
    return 0;
}

static int test_monitor_rejects_bad_options_before_open(void)
{
    char* bad_rate[] = { "monitor", "--rate", "250" };
    char* throttle_log[] = { "monitor", "throttle", "--log", "unused.csv" };
    ASSERT_EQ(run_writable(cmd_monitor, 3, bad_rate), 1);
    ASSERT_EQ(open_calls, 0);
    device.monitor_stop_addr = 0x2000F090;  /* Ends polling if the check is missing. */
    ASSERT_EQ(run_writable(cmd_monitor, 4, throttle_log), 1);
    ASSERT_EQ(open_calls, 0);
    device.monitor_stop_addr = 0;
    return 0;
}

static int test_monitor_sets_up_log_before_polling(void)
{
    char out[512];
    char* invalid[] = { "monitor", "--log", "." };
    monitor_fixture(2, true);
    ASSERT_EQ(run_cli_output(cmd_monitor, 3, invalid, out, sizeof(out)), 1);
    ASSERT_TRUE(strstr(out, "could not open monitor log") != NULL);
    ASSERT_EQ(open_calls, 0);
    ASSERT_EQ(transport_calls, 0);

    char path[] = "/tmp/alltrax-monitor-test-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    char* argv[] = { "monitor", "--log", path };
    device.samples_left = 0; /* Interrupt the first poll before it returns a sample. */
    int rc = run_cli_output(cmd_monitor, 3, argv, out, sizeof(out));
    FILE* file = fopen(path, "r");
    remove(path);
    ASSERT_TRUE(file != NULL);
    char csv[2048];
    size_t n = fread(csv, 1, sizeof(csv) - 1, file);
    fclose(file);
    csv[n] = '\0';
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(strstr(csv, "Model: XCT48400-DCS") != NULL);
    ASSERT_TRUE(strstr(csv, "Date,Time,Keyswitch,") != NULL);
    const char* end = strstr(csv, ",Timestamp\n");
    ASSERT_TRUE(end != NULL);
    ASSERT_STR_EQ(end, ",Timestamp\n");
    return 0;
}

static int test_throttle_list_filters_by_model_and_mask(void)
{
    char out[640];
    char* argv[] = { "throttle", "list" };
    writable_device_reset();
    device.filter_throttles = true;
    device.throttle_mask = (1u << 3) | (1u << 4) | (1u << 7) | (1u << 8);
    ASSERT_EQ(run_cli_output(cmd_throttle, 2, argv, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "   4  EZGO ITS\n   5  Club Car IQ/i2 (MCOR)\n"
                      "   5  Yamaha G19/G22\n   5  0-5K 3 Wire\n   9  Pump\n");
    ASSERT_EQ(device.mutations, 0);

    device.model = "SRX";
    device.filter_throttles = false;
    ASSERT_EQ(run_cli_output(cmd_throttle, 2, argv, out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, "   4  EZGO ITS\n   3  0-5V\n   1  0-5K 2 Wire\n   2  5K-0 2 Wire\n"
                      "   5  0-5K 3 Wire\n   7  5K-0 3 Wire\n   9  Pump\n  10  USB\n  11  Absolute\n");
    ASSERT_EQ(device.mutations, 0);
    return 0;
}

/* The stored name selects among entries sharing the stored type; without a
 * matching name the first entry of that type is current. */
static int test_throttle_list_marks_current_entry(void)
{
    char out[640];
    char* argv[] = { "throttle", "list" };
    writable_device_reset();
    set_page_bytes("Throttle_Type", "\5", 1);
    set_page_bytes("Throttle_Type_Name", "Yamaha G19/G22  ", 16);
    ASSERT_EQ(run_cli_output(cmd_throttle, 2, argv, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "\n*  5  Yamaha G19/G22\n") != NULL);
    ASSERT_TRUE(strstr(out, "\n   5  Club Car IQ/i2 (MCOR)\n") != NULL);

    set_page_bytes("Throttle_Type_Name", "Custom          ", 16);
    ASSERT_EQ(run_cli_output(cmd_throttle, 2, argv, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "\n*  5  Club Car IQ/i2 (MCOR)\n") != NULL);
    ASSERT_TRUE(strstr(out, "\n   5  Yamaha G19/G22\n") != NULL);

    /* Invalid user settings: the factory page is the one in effect. */
    device.page[0] = 0xFF;
    device.factory[0xA0] = 7;
    ASSERT_EQ(run_cli_output(cmd_throttle, 2, argv, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "\n*  7  Club Car PD+ (V-Glide)\n") != NULL);
    ASSERT_EQ(device.mutations, 0);
    return 0;
}

static int test_throttle_set_refuses_ambiguous_unknown_and_unavailable(void)
{
    char out[256];
    char* unknown[] = { "throttle", "set", "unknown" };
    char* ambiguous[] = { "throttle", "set", "Club Car" };
    char* pump[] = { "throttle", "set", "Pump" };
    char* alias[] = { "throttle", "set", "Yamaha G19/G22" };
    writable_device_reset();
    ASSERT_EQ(run_cli_output(cmd_throttle, 3, unknown, out, sizeof(out)), 1);
    ASSERT_TRUE(strstr(out, "unknown throttle") != NULL);
    ASSERT_EQ(open_calls, 0);
    ASSERT_EQ(run_cli_output(cmd_throttle, 3, ambiguous, out, sizeof(out)), 1);
    ASSERT_TRUE(strstr(out, "ambiguous throttle") != NULL);
    ASSERT_EQ(open_calls, 0);
    device.filter_throttles = true;
    device.throttle_mask = 1;
    ASSERT_EQ(run_cli_output(cmd_throttle, 3, pump, out, sizeof(out)), 1);
    ASSERT_TRUE(strstr(out, "not available") != NULL);
    ASSERT_EQ(device.mutations, 0);
    device.model = "SRX";
    device.filter_throttles = false;
    ASSERT_EQ(run_cli_output(cmd_throttle, 3, alias, out, sizeof(out)), 1);
    ASSERT_TRUE(strstr(out, "not available") != NULL);
    ASSERT_EQ(device.mutations, 0);
    return 0;
}

static int test_throttle_set_curve_and_label_in_one_batch(void)
{
    writable_device_reset();
    set_page_bytes("Throttle_Type", "\3", 1);
    char* select[] = { "throttle", "set", "club car iq/i2 (mcor)" };
    ASSERT_EQ(run_writable(cmd_throttle, 3, select), 0);
    ASSERT_EQ(device.erases, 1);
    ASSERT_EQ(device.backup_clears, 1);
    ASSERT_EQ(device.resets, 1);
    ASSERT_EQ(device.page[0xA0], 5);
    ASSERT_EQ(device.page[0xA4], 1);
    ASSERT_MEM_EQ(device.page + 0xC0, "Club Car IQ/i2 (", 16);
    ASSERT_EQ(get_le16(device.page + 0x102), 40); /* Truncated 1% */
    ASSERT_EQ(get_le16(device.page + 0x104), 3276); /* Type 5: 80% */
    ASSERT_EQ(get_le16(device.page + 0x124), 4095);

    /* Selecting the same type again also installs its default curve. */
    char* same[] = { "throttle", "set", "Yamaha G19" };
    device.page[0x104] = 123;
    device.page[0x105] = 0;
    ASSERT_EQ(run_writable(cmd_throttle, 3, same), 0);
    ASSERT_MEM_EQ(device.page + 0xC0, "Yamaha G19/G22  ", 16);
    ASSERT_EQ(get_le16(device.page + 0x104), 3276);
    return 0;
}

static int test_throttle_set_after_factory_recovery(void)
{
    writable_device_reset();
    device.page[0] = 0xFF;
    device.page[0xA0] = 3;
    device.factory[0xA0] = 5;
    device.factory[0x104] = 123;
    char* argv[] = { "throttle", "set", "Yamaha G19/G22" };
    ASSERT_EQ(run_writable(cmd_throttle, 3, argv), 0);
    ASSERT_EQ(device.erases, 2);
    ASSERT_EQ(get_le16(device.page + 0x104), 3276);
    ASSERT_EQ(device.resets, 1);
    return 0;
}

static int test_throttle_set_uses_type_specific_preset(void)
{
    const struct { char* label; uint16_t x1, x2, y2; } cases[] = {
        { "0-5K 2 Wire", 40, 532, 81 },
        { "5K-0 2 Wire", 40, 368, 1105 },
        { "5K-0 3 Wire", 204, 4054, 4095 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        writable_device_reset();
        char* argv[] = { "throttle", "set", cases[i].label };
        ASSERT_EQ(run_writable(cmd_throttle, 3, argv), 0);
        ASSERT_EQ(get_le16(device.page + 0x102), cases[i].x1);
        ASSERT_EQ(get_le16(device.page + 0x104), cases[i].x2);
        ASSERT_EQ(get_le16(device.page + 0x124), cases[i].y2);
    }
    return 0;
}

static int test_throttle_set_absolute_installs_linear_curve(void)
{
    char out[256];
    writable_device_reset();
    set_page_bytes("Throttle_Type", "\13", 1);
    device.page[0x104] = 123;
    device.page[0x105] = 0;
    char* same[] = { "throttle", "set", "Absolute" };
    ASSERT_EQ(run_cli_output(cmd_throttle, 3, same, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "Set throttle to Absolute (11), HPD on") != NULL);
    ASSERT_EQ(get_le16(device.page + 0x104), 4054);
    return 0;
}

static int test_throttle_set_disables_hpd_for_pump_and_usb(void)
{
    char* labels[] = { "Pump", "USB", "Absolute" };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        writable_device_reset();
        set_page_bytes("Throttle_Type", "\3", 1);
        set_page_bytes("HPD", "\1", 1);
        set_page_bytes("ABS_Lo_Throt_Min", "\x34\x02", 2);
        char* argv[] = { "throttle", "set", labels[i] };
        ASSERT_EQ(run_writable(cmd_throttle, 3, argv), 0);
        ASSERT_EQ(device.page[0xA0], 9 + i);
        ASSERT_EQ(device.page[0xA4], i == 2);
        ASSERT_EQ(get_le16(device.page + 0xAA), 0x0234);
        ASSERT_EQ(get_le16(device.page + 0x104), 4054); /* Linear: truncated 99% */
        ASSERT_EQ(device.resets, 1);
    }
    return 0;
}

static int test_restore_defaults_cli_restores_everything(void)
{
    /* A restore replaces the throttle settings and the field map, so it needs
     * both confirmations. */
    char* plain[] = { "restore-defaults" };
    char* throttle_only[] = { "restore-defaults", "--throttle" };
    char* map_only[] = { "restore-defaults", "--field-map" };
    writable_device_reset();
    open_calls = 0;
    ASSERT_EQ(run_writable(cmd_restore_defaults, 1, plain), 1);
    ASSERT_EQ(run_writable(cmd_restore_defaults, 2, throttle_only), 1);
    ASSERT_EQ(run_writable(cmd_restore_defaults, 2, map_only), 1);
    ASSERT_EQ(open_calls, 0);

    char* argv[] = { "restore-defaults", "--field-map", "--throttle" };
    writable_device_reset();
    set_page_bytes("Throttle_Type", "\3", 1);
    set_page_bytes("F_Table_Name", "MINE", 4);
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, device.factory, sizeof(expected));
    expected[0] = expected[1] = 0;
    reconnect_waits = 0;
    ASSERT_EQ(run_writable(cmd_restore_defaults, 3, argv), 0);
    ASSERT_MEM_EQ(device.page, expected, VARSET_SIZE);
    ASSERT_EQ(device.erases, 1);
    ASSERT_EQ(device.backup_clears, 1);  /* The throttle type changed. */
    ASSERT_EQ(device.resets, 1);
    ASSERT_EQ(reconnect_waits, 1);

    char* extra[] = { "restore-defaults", "unexpected", "--throttle", "--field-map" };
    open_calls = 0;
    ASSERT_EQ(run_writable(cmd_restore_defaults, 4, extra), 1);
    ASSERT_EQ(open_calls, 0);
    return 0;
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

static int test_named_writes_need_no_confirmation(void)
{
    char turbo[] = "Turbo_Start_RPM=2000", type[] = "Throttle_Type=1";
    char point0[] = "0,0", point1[] = "750,50";
    char* write_scalar[] = { "write", turbo };
    char* write_type[] = { "write", type };
    char* set_field[] = { "curve", "set", "field", point0, point1 };
    char* reset_gen[] = { "curve", "reset", "genfield" };
    char* reset_lin[] = { "curve", "reset", "linearization" };
    struct { int (*cmd)(int, char**); int argc; char** argv; } cases[] = {
        { cmd_write, 2, write_scalar }, { cmd_write, 2, write_type },
        { cmd_curve, 5, set_field }, { cmd_curve, 3, reset_gen },
        { cmd_curve, 3, reset_lin },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        writable_device_reset();
        ASSERT_EQ(run_writable(cases[i].cmd, cases[i].argc, cases[i].argv), 0);
        ASSERT_EQ(device.erases, 1);
    }
    return 0;
}

/* curve reset all writes both groups, so it needs both confirmations. */
static int test_curve_reset_all_requires_both_confirmations(void)
{
    char* none[] = { "curve", "reset", "all" };
    char* throttle[] = { "curve", "reset", "all", "--throttle" };
    char* field_map[] = { "curve", "reset", "all", "--field-map" };
    char** refused[] = { none, throttle, field_map };
    const int argc[] = { 3, 4, 4 };
    for (size_t i = 0; i < 3; i++) {
        writable_device_reset();
        ASSERT_EQ(run_writable(cmd_curve, argc[i], refused[i]), 1);
        ASSERT_EQ(open_calls, 0);
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
    char* argv[] = { "write", arg };
    ASSERT_EQ(run_writable(cmd_write, 2, argv), 0);
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
    char* argv[] = { "write", arg };
    ASSERT_EQ(run_writable(cmd_write, 2, argv), 0);
    ASSERT_EQ(device.erases, 2);
    uint8_t expected[32];
    ASSERT_EQ(alltrax_encode_string(name, "FACTORY", expected), 32);
    ASSERT_MEM_EQ(device.page + offset, expected, sizeof(expected));
    return 0;
}

#ifdef ALLTRAX_HAVE_XML
/* A field-map file: map A in versions 1 and 2 (2 has aliases OLDA and OTHER),
 * map B without a flag line, and a hidden map. */
static const char map_file_text[] =
    "F001.002.003.004\r\n"
    "----- ----- ----- ----- -----\r\n"
    "MAPA\r\n" "1\r\n" "\r\n" "\r\n"
    "<XCT>\r\n"
    "  <Node-Field><F_Table_Name>MAPA-V1</F_Table_Name>"
    "<Turbo_Start_RPM>1000</Turbo_Start_RPM></Node-Field>\r\n"
    "</XCT>\r\n"
    "----- ----- ----- ----- -----\r\n"
    "MAPA\r\n" "2\r\n" "OLDA\\OTHER\r\n" "\r\n"
    "<XCT>\r\n"
    "  <Node-Tach><Tach_4_8>1</Tach_4_8></Node-Tach>\r\n"
    "  <Node-Field><F_Table_Name>MAPA-V2</F_Table_Name>"
    "<Turbo_Start_RPM>2000</Turbo_Start_RPM></Node-Field>\r\n"
    "</XCT>\r\n"
    "----- ----- ----- ----- -----\r\n"
    "MAPB\r\n" "0\r\n" "\r\n"
    "<XCT>\r\n"
    "  <Node-Tach><Tach_4_8>1</Tach_4_8></Node-Tach>\r\n"
    "  <Node-Field><F_Table_Name>MAPB</F_Table_Name>"
    "<Turbo_Start_RPM>3000</Turbo_Start_RPM></Node-Field>\r\n"
    "</XCT>\r\n"
    "----- ----- ----- ----- -----\r\n"
    "HIDE\r\n" "5\r\n" "\r\n" "HIDDEN\r\n"
    "<XCT><Node-Field><F_Table_Name>HIDE</F_Table_Name></Node-Field></XCT>\r\n";

/* Run a field-map subcommand on the writable device with a plain map file
 * holding `text`. args follow the subcommand; the file path comes first. */
static int run_map_command(const char* text, const char* const* args, size_t n,
    char* out, size_t out_size)
{
    char path[64];
    if (make_temp_path(path) || write_file(path, text)) return -1;
    char* argv[12] = { "field-map", (char*)args[0], path, "--key", "none" };
    int argc = 5;
    for (size_t i = 1; i < n && argc < 12; i++)
        argv[argc++] = (char*)args[i];
    char discard[4096];
    int rc = run_cli_output(cmd_field_map, argc, argv, out ? out : discard,
                         out ? out_size : sizeof(discard));
    remove(path);
    return rc;
}

/* The settings page after writing a map's name and Turbo_Start_RPM, and
 * Tach_4_8 when tach >= 0. */
static void expected_map_page(uint8_t page[VARSET_SIZE], const char* name,
    double turbo, int tach)
{
    const alltrax_var_def* name_var = alltrax_find_var("F_Table_Name");
    const alltrax_var_def* turbo_var = alltrax_find_var("Turbo_Start_RPM");
    const alltrax_var_def* tach_var = alltrax_find_var("Tach_4_8");
    alltrax_encode_string(name_var, name, page + (name_var->address - VARSET_ADDR));
    alltrax_encode_var(turbo_var, turbo, page + (turbo_var->address - VARSET_ADDR));
    if (tach >= 0)
        alltrax_encode_var(tach_var, tach, page + (tach_var->address - VARSET_ADDR));
}

static int test_field_map_list_marks_current_map_and_upgrade(void)
{
    static char out[4096];
    const char* args[] = { "list" };
    writable_device_reset();
    set_page_string("F_Table_Name", "MAPA-V1");
    ASSERT_EQ(run_map_command(map_file_text, args, 1, out, sizeof(out)), 0);
    ASSERT_EQ(device.mutations, 0);
    ASSERT_TRUE(strstr(out, "file version 1.2.3.4, 3 maps") != NULL);
    ASSERT_TRUE(strstr(out, "* MAPA ") != NULL);
    ASSERT_TRUE(strstr(out, "  MAPB ") != NULL);
    ASSERT_TRUE(strstr(out, "version 2  aliases: OLDA, OTHER\n") != NULL);
    ASSERT_NULL(strstr(out, "HIDE"));
    ASSERT_TRUE(strstr(out, "Controller map: MAPA-V1\nUpgrade available: MAPA version 2\n") != NULL);

    /* Invalid user settings leave the controller's map unknown. */
    writable_device_reset();
    set_page_string("F_Table_Name", "MAPA-V1");
    device.page[0] = device.page[1] = 0xFF;
    ASSERT_EQ(run_map_command(map_file_text, args, 1, out, sizeof(out)), 0);
    ASSERT_NULL(strstr(out, "* MAPA"));
    ASSERT_TRUE(strstr(out, "Controller map: unknown") != NULL);
    ASSERT_TRUE(strstr(out, "No upgrade available") != NULL);
    return 0;
}

/* A record with a bad version, or too few lines, rejects the whole file. */
static const char broken_version[] =
    "----- ----- ----- ----- -----\r\nBROKEN\r\nx\r\n\r\n\r\n<XCT></XCT>\r\n";
static const char truncated_record[] =
    "----- ----- ----- ----- -----\r\nBROKEN\r\n1\r\n";

static int test_field_map_file_errors_stop_before_open(void)
{
    static char broken[2][sizeof(map_file_text) + sizeof(broken_version)];
    snprintf(broken[0], sizeof(broken[0]), "%s%s", map_file_text, broken_version);
    snprintf(broken[1], sizeof(broken[1]), "%s%s", map_file_text, truncated_record);
    const char* bad_files[] = {
        broken[0], broken[1],
        "X01.002.003.004\r\n----- ----- ----- ----- -----\r\nMAPA\r\n1\r\n\r\n\r\n<XCT/>\r\n",
        "F001.002.003\r\n",
        "F001.002.003.004\r\n----- ----- ----- ----- -----\r\nHIDE\r\n1\r\n\r\nHIDDEN\r\n<XCT/>\r\n",
    };
    const char* list[] = { "list" };
    for (size_t i = 0; i < sizeof(bad_files) / sizeof(bad_files[0]); i++) {
        writable_device_reset();
        ASSERT_EQ(run_map_command(bad_files[i], list, 1, NULL, 0), 1);
        ASSERT_EQ(open_calls, 0);
    }

    char missing[] = "/nonexistent/alltrax-fields.txt";
    char bad_key[] = "0123";
    char* no_file[] = { "field-map", "list", missing, "--key", "none" };
    char* key[] = { "field-map", "list", missing, "--key", bad_key };
    char* usage[] = { "field-map", "install", missing };
    ASSERT_EQ(run_writable(cmd_field_map, 5, no_file), 1);
    ASSERT_EQ(open_calls, 0);
    ASSERT_EQ(run_writable(cmd_field_map, 5, key), 1);
    ASSERT_EQ(open_calls, 0);
    ASSERT_EQ(run_writable(cmd_field_map, 3, usage), 1);
    ASSERT_EQ(open_calls, 0);

    /* Without "none", a plain file is never read as plain text. */
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    ASSERT_EQ(write_file(path, map_file_text), 0);
    unsetenv("ALLTRAX_FIELDMAP_KEY");
    char* auto_key[] = { "field-map", "list", path };
    int rc = run_writable(cmd_field_map, 3, auto_key);
    remove(path);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(open_calls, 0);
    return 0;
}

static int test_field_map_upgrade_selects_newer_version(void)
{
    const struct {
        const char* current;
        int rc;
        const char* name;   /* map written, or NULL for no write */
    } cases[] = {
        { "MAPA-V1", 0, "MAPA-V2" },
        { "OLDA", 0, "MAPA-V2" },      /* alias of version 2 */
        { "MAPA-V2", 0, NULL },        /* already current */
        { "MAPB", 0, NULL },           /* version 0 is never an upgrade */
        { "NOPE", 0, NULL },
        { "MAPA-Vx", 1, NULL },
    };
    const char* args[] = { "upgrade" };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        writable_device_reset();
        set_page_string("F_Table_Name", cases[i].current);
        uint8_t expected[VARSET_SIZE];
        memcpy(expected, device.page, sizeof(expected));
        if (cases[i].name)
            expected_map_page(expected, cases[i].name, 2000, -1);
        device.mutations = 0;
        ASSERT_EQ(run_map_command(map_file_text, args, 1, NULL, 0), cases[i].rc);
        ASSERT_EQ(device.erases, cases[i].name ? 1u : 0u);
        ASSERT_EQ(device.resets, 0);
        if (!cases[i].name)
            ASSERT_EQ(device.mutations, 0);
        ASSERT_MEM_EQ(device.page, expected, sizeof(expected));  /* Tach_4_8 kept */
    }

    writable_device_reset();
    set_page_string("F_Table_Name", "MAPA-V1");
    device.page[0] = device.page[1] = 0xFF;
    ASSERT_EQ(run_map_command(map_file_text, args, 1, NULL, 0), 1);
    ASSERT_EQ(device.mutations, 0);
    return 0;
}

static int test_field_map_install_writes_named_map_and_pole_count(void)
{
    static char out[4096];
    const char* hidden[] = { "install", "HIDE" };
    const char* unknown[] = { "install", "NOPE" };
    const char* const* refused[] = { hidden, unknown };
    for (size_t i = 0; i < 2; i++) {
        writable_device_reset();
        ASSERT_EQ(run_map_command(map_file_text, refused[i], 2, NULL, 0), 1);
        ASSERT_EQ(open_calls, 0);
    }

    const struct { const char* name; const char* map; double turbo; } cases[] = {
        { "MAPB", "MAPB", 3000 },
        { "MAPA", "MAPA-V2", 2000 },   /* highest version */
        { "OTHER", "MAPA-V2", 2000 },  /* alias */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        writable_device_reset();
        uint8_t expected[VARSET_SIZE];
        memcpy(expected, device.page, sizeof(expected));
        expected_map_page(expected, cases[i].map, cases[i].turbo, 1);
        const char* args[] = { "install", cases[i].name };
        ASSERT_EQ(run_map_command(map_file_text, args, 2, out, sizeof(out)), 0);
        ASSERT_EQ(device.erases, 1);
        ASSERT_EQ(device.resets, 1);  /* Tach_4_8 reboots the controller */
        ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
        ASSERT_TRUE(strstr(out, "Speed sensor pole count (Tach_4_8): 4-pole -> 8-pole") != NULL);
        ASSERT_TRUE(strstr(out, "must match the installed motor") != NULL);
    }
    return 0;
}

#ifdef ALLTRAX_HAVE_CRYPTO
static int write_encrypted(const char* path, const char* text, const char* key_text)
{
    cli_key key;
    if (cli_resolve_key("test", key_text, "ALLTRAX_TEST_UNSET", NULL, &key))
        return -1;
    static unsigned char data[8192];
    int len = 0, total = 0;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_des_ede3_cbc(), NULL, key.key, key.iv) &&
              EVP_EncryptUpdate(ctx, data, &len, (const unsigned char*)text, (int)strlen(text));
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, data + total, &len);
    EVP_CIPHER_CTX_free(ctx);
    FILE* f = ok ? fopen(path, "wb") : NULL;
    if (!f) return -1;
    ok = fwrite(data, 1, (size_t)(total + len), f) == (size_t)(total + len);
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* An explicit key wins over ALLTRAX_FIELDMAP_KEY; a wrong key or reading an
 * encrypted file as plain text fails before the controller is opened. */
static int test_fieldmap_key_decrypts_map_file(void)
{
    static char out[4096];
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    ASSERT_EQ(write_encrypted(path, map_file_text, TEST_KEY_A), 0);
    char key_a[] = TEST_KEY_A, key_b[] = TEST_KEY_B;
    char* right[] = { "field-map", "list", path, "--key", key_a };
    char* wrong[] = { "field-map", "list", path, "--key", key_b };
    char* plain[] = { "field-map", "list", path, "--key", "none" };
    char* env[] = { "field-map", "list", path };
    writable_device_reset();
    ASSERT_EQ(run_cli_output(cmd_field_map, 5, right, out, sizeof(out)), 0);
    ASSERT_TRUE(strstr(out, "3 maps") != NULL);
    ASSERT_EQ(run_cli_output(cmd_field_map, 5, wrong, out, sizeof(out)), 1);
    ASSERT_EQ(open_calls, 0);
    ASSERT_EQ(run_cli_output(cmd_field_map, 5, plain, out, sizeof(out)), 1);
    ASSERT_EQ(open_calls, 0);
    setenv("ALLTRAX_FIELDMAP_KEY", TEST_KEY_A, 1);
    ASSERT_EQ(run_cli_output(cmd_field_map, 3, env, out, sizeof(out)), 0);
    setenv("ALLTRAX_FIELDMAP_KEY", TEST_KEY_B, 1);
    ASSERT_EQ(run_cli_output(cmd_field_map, 5, right, out, sizeof(out)), 0);
    unsetenv("ALLTRAX_FIELDMAP_KEY");

    /* A plain file given a key gets a hint. */
    ASSERT_EQ(write_file(path, map_file_text), 0);
    ASSERT_EQ(run_cli_output(cmd_field_map, 5, right, out, sizeof(out)), 1);
    ASSERT_TRUE(strstr(out, "use --key none") != NULL);
    remove(path);
    return 0;
}
#endif

#ifdef ALLTRAX_HAVE_CRYPTO
static int test_config_key_load_round_trip(void)
{
    char out[512];
    char path[64];
    unsetenv("ALLTRAX_CONFIG_KEY");
    writable_device_reset();
    ASSERT_EQ(make_temp_path(path), 0);
    ASSERT_EQ(run_config_save_as(path, 5005, TEST_KEY_A), 0);

    char* wrong[] = { "config", "load", path, "--key", TEST_KEY_B, "--throttle", "--field-map" };
    char* plain[] = { "config", "load", path, "--key", "none", "--throttle", "--field-map" };
    char* right[] = { "config", "load", path, "--key", TEST_KEY_A, "--throttle", "--field-map" };
    device.erases = device.mutations = 0;
    ASSERT_EQ(run_cli_output(cmd_config, 7, wrong, out, sizeof(out)), 1);
    ASSERT_EQ(run_cli_output(cmd_config, 7, plain, out, sizeof(out)), 1);
    ASSERT_EQ(device.mutations, 0);
    ASSERT_EQ(run_cli_output(cmd_config, 7, right, out, sizeof(out)), 0);
    ASSERT_EQ(device.erases, 1);

    /* A plain file given a key gets a hint. */
    ASSERT_EQ(run_config_save_as(path, 5005, "none"), 0);
    ASSERT_EQ(run_cli_output(cmd_config, 5, right, out, sizeof(out)), 1);
    ASSERT_TRUE(strstr(out, "use --key none") != NULL);
    remove(path);
    return 0;
}
#endif

/* Load a plain XML file into the writable device, confirming throttle and
 * field-map settings as asked. */
static int run_config_load(const char* xml, bool throttle, bool field_map)
{
    char path[64];
    if (make_temp_path(path) || write_file(path, xml)) return -1;
    char* argv[7] = { "config", "load", "--key", "none", path };
    int argc = 5;
    if (throttle) argv[argc++] = "--throttle";
    if (field_map) argv[argc++] = "--field-map";
    int rc = run_writable(cmd_config, argc, argv);
    remove(path);
    return rc;
}

/* A file with throttle or field-map settings loads only with the matching
 * confirmations; a file without them needs none. */
static int test_config_load_requires_group_confirmations(void)
{
    const char* xml = "<XCT><Node-Throttle><HPD>1</HPD></Node-Throttle>"
                      "<Node-Field><Turbo_Start_RPM>2000</Turbo_Start_RPM>"
                      "</Node-Field></XCT>";
    for (int flags = 0; flags < 3; flags++) {
        writable_device_reset();
        ASSERT_EQ(run_config_load(xml, flags == 1, flags == 2), 1);
        ASSERT_EQ(open_calls, 0);
    }
    writable_device_reset();
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, device.page, sizeof(expected));
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    const alltrax_var_def* turbo = alltrax_find_var("Turbo_Start_RPM");
    expected[hpd->address - VARSET_ADDR] = 1;
    alltrax_encode_var(turbo, 2000, expected + (turbo->address - VARSET_ADDR));
    ASSERT_EQ(run_config_load(xml, true, true), 0);
    ASSERT_MEM_EQ(device.page, expected, sizeof(expected));

    writable_device_reset();
    ASSERT_EQ(run_config_load("<XCT><Node-Voltage><LoBat_Vlim>210</LoBat_Vlim>"
                              "</Node-Voltage></XCT>", false, false), 0);
    ASSERT_EQ(device.erases, 1);
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
        ASSERT_EQ(run_config_load(xml, true, false), 1);
        ASSERT_EQ(open_calls, 0);
    }
    writable_device_reset();
    ASSERT_EQ(run_config_load("<XCT><Node-Throttle><HPD> 1 </HPD></Node-Throttle></XCT>",
                              true, false), 0);
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

/* ------------------------------------------------------------------ */
/* Wire sequences                                                     */
/* ------------------------------------------------------------------ */

/* Expected device mutations (every write, erase and reset request), in
 * order. Reads are checked separately: a full page read must come before each
 * page erase on the settings paths and before every GoodSet clear. */
static struct {
    uint8_t packet[WIRE_LOG_MAX][PACKET_SIZE];
    size_t count;
    size_t date_index;  /* 1-based position of the FieldMap_Date write */
} script;

static void begin_script(void)
{
    memset(&script, 0, sizeof(script));
}

static uint32_t wire_addr(const uint8_t* p)
{
    return ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
           ((uint32_t)p[6] << 8) | p[7];
}

static void expect_packet(uint8_t report, uint32_t addr, const uint8_t* data, size_t len)
{
    uint8_t* p = script.packet[script.count++];
    memset(p, 0, PACKET_SIZE);
    p[0] = report;
    p[1] = (uint8_t)len;
    p[4] = (uint8_t)(addr >> 24);
    p[5] = (uint8_t)(addr >> 16);
    p[6] = (uint8_t)(addr >> 8);
    p[7] = (uint8_t)addr;
    if (len) memcpy(p + 8, data, len);
}

static void expect_mode(uint8_t mode)
{
    expect_packet(2, 0x2000FFFA, &mode, 1);
}

static size_t page_chunk(size_t offset)
{
    return VARSET_SIZE - offset < 56 ? VARSET_SIZE - offset : 56;
}

/* Erase page 4, program the page in 56-byte chunks with GoodSet still erased,
 * then clear GoodSet. */
static void expect_page_cycle(const uint8_t page[VARSET_SIZE])
{
    const uint8_t erase[] = { 0, 0, 0, 4 };
    expect_packet(3, 1, erase, sizeof(erase));
    uint8_t staged[VARSET_SIZE];
    memcpy(staged, page, sizeof(staged));
    staged[0] = staged[1] = 0xFF;
    for (size_t offset = 0; offset < VARSET_SIZE; offset += 56)
        expect_packet(2, 0x08002000 + (uint32_t)offset, staged + offset, page_chunk(offset));
    const uint8_t valid[2] = { 0, 0 };
    expect_packet(2, 0x08002000, valid, sizeof(valid));
}

static void expect_settings_write(const uint8_t page[VARSET_SIZE])
{
    expect_mode(0xFF);
    expect_page_cycle(page);
    expect_mode(0x00);
}

static void expect_backup_clear(void)
{
    const uint8_t zeros[20] = {0};
    expect_packet(2, 0x40006C00, zeros, sizeof(zeros));
}

static void expect_reset(void)
{
    expect_packet(3, 12, NULL, 0);
}

static void expect_date(void)
{
    const uint8_t unknown[4] = {0};
    expect_packet(2, 0x2000F210, unknown, sizeof(unknown));
    script.date_index = script.count;
}

/* True when the requests just before log entry `end` read the whole page. */
static bool page_read_before(size_t end)
{
    size_t chunks = (VARSET_SIZE + 55) / 56;
    if (end < chunks) return false;
    for (size_t i = 0; i < chunks; i++) {
        const uint8_t* p = device.wire[end - chunks + i];
        if (p[0] != 1 || p[1] != page_chunk(i * 56) ||
            wire_addr(p) != 0x08002000 + i * 56)
            return false;
    }
    return true;
}

static int check_wire(bool page_read_before_erase)
{
    ASSERT_TRUE(device.wire_count <= WIRE_LOG_MAX);
    size_t n = 0;
    for (size_t i = 0; i < device.wire_count; i++) {
        const uint8_t* p = device.wire[i];
        if (p[0] == 1) continue;
        ASSERT_TRUE(n < script.count);
        const uint8_t* want = script.packet[n++];
        if (n == script.date_index) {
            time_t now = time(NULL);
            struct tm date;
            ASSERT_NOT_NULL(localtime_r(&now, &date));
            unsigned year = (unsigned)date.tm_year + 1900;
            const uint8_t today[4] = { (uint8_t)year, (uint8_t)(year >> 8),
                (uint8_t)date.tm_mday, (uint8_t)(date.tm_mon + 1) };
            ASSERT_MEM_EQ(p, want, 8);
            ASSERT_MEM_EQ(p + 8, today, sizeof(today));
            ASSERT_MEM_EQ(p + 12, want + 12, PACKET_SIZE - 12);
        } else {
            ASSERT_MEM_EQ(p, want, PACKET_SIZE);
        }
        if (p[0] == 3 && wire_addr(p) == 1 && page_read_before_erase)
            ASSERT_TRUE(page_read_before(i));
        if (p[0] == 2 && wire_addr(p) == 0x08002000 && p[1] == 2)
            ASSERT_TRUE(page_read_before(i));
    }
    ASSERT_EQ(n, script.count);
    return 0;
}

static void put_curve(uint8_t page[VARSET_SIZE], const char* name,
    const uint16_t* x, const uint16_t* y, size_t n)
{
    const alltrax_curve_def* def = alltrax_find_curve(name);
    for (size_t i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
        uint16_t xv = i < n ? x[i] : 0, yv = i < n ? y[i] : 0;
        uint8_t* px = page + (def->x_address - VARSET_ADDR) + i * 2;
        uint8_t* py = page + (def->y_address - VARSET_ADDR) + i * 2;
        px[0] = (uint8_t)xv; px[1] = (uint8_t)(xv >> 8);
        py[0] = (uint8_t)yv; py[1] = (uint8_t)(yv >> 8);
    }
}

static const uint16_t linear_x[] = { 0, 40, 4054, 4095 };
static const uint16_t linear_y[] = { 0, 0, 4095, 4095 };

/* EZGO ITS selected, HPD on, its default linearization installed. */
static void ezgo_page(void)
{
    writable_device_reset();
    set_page_bytes("Throttle_Type", "\4", 1);
    set_page_bytes("HPD", "\1", 1);
    set_page_bytes("Throttle_Type_Name", "EZGO ITS        ", 16);
    put_curve(device.page, "linearization", linear_x, linear_y, 4);
}

static int test_setting_write_wire_sequence(void)
{
    writable_device_reset();
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, device.page, sizeof(expected));
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    ASSERT_EQ(alltrax_encode_var(var, 20.1, expected + (var->address - VARSET_ADDR)), 2);

    char setting[] = "LoBat_Vlim=20.1";
    char* argv[] = { "write", setting };
    ASSERT_EQ(run_writable(cmd_write, 2, argv), 0);
    ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
    begin_script();
    expect_settings_write(expected);
    expect_reset();
    return check_wire(true);
}

static int test_reset_trigger_write_wire_sequence(void)
{
    writable_device_reset();
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, device.page, sizeof(expected));

    char speed[] = "Speed_Limit_On=0", tach[] = "Tach_4_8=0";
    char* argv[] = { "write", speed, tach };
    ASSERT_EQ(run_writable(cmd_write, 3, argv), 0);
    ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
    begin_script();
    expect_settings_write(expected);
    expect_reset();
    return check_wire(true);
}

static int run_throttle_select(char* label, const uint8_t expected[VARSET_SIZE])
{
    device.wire_count = 0;
    char* argv[] = { "throttle", "set", label };
    ASSERT_EQ(run_writable(cmd_throttle, 3, argv), 0);
    ASSERT_MEM_EQ(device.page, expected, VARSET_SIZE);
    begin_script();
    expect_settings_write(expected);
    expect_backup_clear();
    expect_reset();
    return check_wire(true);
}

static int test_throttle_select_wire_sequences(void)
{
    ezgo_page();
    uint8_t ezgo[VARSET_SIZE], club[VARSET_SIZE];
    memcpy(ezgo, device.page, sizeof(ezgo));

    /* Selecting the installed throttle rewrites the page unchanged. */
    if (run_throttle_select("EZGO ITS", ezgo)) return 1;

    /* A new type changes the type, the 16-byte label and its default curve. */
    memcpy(club, ezgo, sizeof(club));
    club[0xA0] = 5;
    memcpy(club + 0xC0, "Club Car IQ/i2 (", 16);
    club[0x104] = 0xCC;  /* 3276: 80 % */
    club[0x105] = 0x0C;
    if (run_throttle_select("Club Car IQ/i2 (MCOR)", club)) return 1;
    return run_throttle_select("EZGO ITS", ezgo);
}

static int test_throttle_reselect_installs_default_curve_wire_sequence(void)
{
    const uint16_t custom_x[] = { 0, 100, 1000, 2000, 4095 };
    const uint16_t custom_y[] = { 0, 0, 1500, 3000, 4095 };
    writable_device_reset();
    set_page_bytes("Throttle_Type", "\4", 1);
    set_page_bytes("HPD", "\1", 1);
    put_curve(device.page, "linearization", custom_x, custom_y, 5);
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, device.page, sizeof(expected));
    memcpy(expected + 0xC0, "EZGO ITS", 8);
    put_curve(expected, "linearization", linear_x, linear_y, 4);

    char* argv[] = { "throttle", "set", "EZGO ITS" };
    ASSERT_EQ(run_writable(cmd_throttle, 3, argv), 0);
    ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
    begin_script();
    expect_settings_write(expected);
    expect_backup_clear();
    expect_reset();
    return check_wire(true);
}

static int test_restore_defaults_wire_sequence(void)
{
    ezgo_page();
    memcpy(device.factory, device.page, VARSET_SIZE);
    const alltrax_var_def* lobat = alltrax_find_var("LoBat_Vlim");
    ASSERT_EQ(alltrax_encode_var(lobat, 21, device.factory + (lobat->address - VARSET_ADDR)), 2);
    memset(device.factory + 0xC0, ' ', 16);
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, device.factory, sizeof(expected));

    /* The throttle type and HPD do not change, so calibration is kept. */
    char* argv[] = { "restore-defaults", "--throttle", "--field-map" };
    ASSERT_EQ(run_writable(cmd_restore_defaults, 3, argv), 0);
    ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
    begin_script();
    expect_page_cycle(expected);
    expect_date();
    expect_reset();
    return check_wire(false);
}

/* Compare the RAM reads in the log with (address, length) pairs. */
static int check_ram_reads(const uint32_t want[][2], size_t count)
{
    size_t n = 0;
    for (size_t i = 0; i < device.wire_count && i < WIRE_LOG_MAX; i++) {
        const uint8_t* p = device.wire[i];
        uint32_t addr = wire_addr(p);
        if (p[0] != 1 || addr < 0x2000F000 || addr >= 0x2000F200) continue;
        ASSERT_TRUE(n < count);
        ASSERT_EQ(addr, want[n][0]);
        ASSERT_EQ(p[1], want[n][1]);
        n++;
    }
    ASSERT_EQ(n, count);
    return 0;
}

static int test_monitor_user_inputs_read_block_and_bench_values(void)
{
    /* One sample, then the first read of the next one is interrupted. */
    const uint32_t blocks[][2] = {
        { 0x2000F000, 17 }, { 0x2000F030, 34 }, { 0x2000F090, 17 },
        { 0x2000F0E6, 2 }, { 0x2000F110, 56 }, { 0x2000F148, 30 },
        { 0x2000F000, 17 },
    };
    char out[4096];
    char* argv[] = { "monitor", "--rate", "100" };
    for (unsigned state = 1; state <= 2; state++) {
        monitor_fixture(state, true);
        monitor_word(0x160, 0);       /* Normal profile */
        monitor_word(0x162, 0xFFFF);  /* Nothing connected */
        monitor_word(0x164, 0xFFFF);
        ASSERT_EQ(run_cli_output(cmd_monitor, 3, argv, out, sizeof(out)), 0);
        if (check_ram_reads(blocks, sizeof(blocks) / sizeof(blocks[0]))) return 1;
        ASSERT_TRUE(strstr(out, "User1 (Regen)         Disconnected") != NULL);
        ASSERT_TRUE(strstr(out, state == 1 ? "Selected Profile      Normal"
                                          : "User2 (Speed)         Disconnected") != NULL);
    }
    return 0;
}

static int test_monitor_throttle_view_read_blocks(void)
{
    const uint32_t blocks[][2] = {
        { 0x2000F090, 1 }, { 0x2000F116, 20 }, { 0x2000F15E, 2 }, { 0x2000F090, 1 },
    };
    char out[512];
    char* argv[] = { "monitor", "throttle" };
    monitor_fixture(0, true);
    device.monitor_stop_addr = 0x2000F090;
    ASSERT_EQ(run_cli_output(cmd_monitor, 2, argv, out, sizeof(out)), 0);
    return check_ram_reads(blocks, sizeof(blocks) / sizeof(blocks[0]));
}

#ifdef ALLTRAX_HAVE_XML
static int run_config_load_file(const char* path)
{
    device.wire_count = 0;
    char* argv[] = { "config", "load", "--key", "none", (char*)path, "--throttle", "--field-map" };
    return run_writable(cmd_config, 7, argv);
}

static int test_config_load_wire_sequence(void)
{
    ezgo_page();
    uint8_t saved[VARSET_SIZE];
    memcpy(saved, device.page, sizeof(saved));
    char path[64];
    ASSERT_EQ(make_temp_path(path), 0);
    ASSERT_EQ(run_config_save(path, 5005), 0);

    /* Change a profile setting, the label and the curve, then load the file. */
    const alltrax_var_def* roll = alltrax_find_var("N_RollDetect_BrakingCurrent");
    ASSERT_EQ(alltrax_encode_var(roll, -50, device.page + (roll->address - VARSET_ADDR)), 2);
    memset(device.page + 0xC0, ' ', 16);
    put_curve(device.page, "linearization", linear_y, linear_x, 4);
    int rc = run_config_load_file(path);
    remove(path);
    ASSERT_EQ(rc, 0);
    ASSERT_MEM_EQ(device.page, saved, sizeof(saved));
    begin_script();
    expect_settings_write(saved);
    expect_date();  /* The file includes the field map. */
    expect_backup_clear();
    expect_reset();
    return check_wire(true);
}

#define CURVE_ZEROS ",0,0,0,0,0,0,0,0,0,0,0,0"
static const char throttle_node_config[] =
    "<XCT>\n  <Node-Throttle>\n"
    "    <Throttle_Type>4</Throttle_Type>\n"
    "    <Throttle_Type_Name>EZGO ITS        </Throttle_Type_Name>\n"
    "    <HPD>1</HPD>\n"
    "    <Lin_Table_Pointer>0,40,4054,4095" CURVE_ZEROS "</Lin_Table_Pointer>\n"
    "    <Lin_Table_Data>0,0,4095,4095" CURVE_ZEROS "</Lin_Table_Data>\n"
    "    <V_Table_Pointer>0,100,2000,4095" CURVE_ZEROS "</V_Table_Pointer>\n"
    "    <V_Table_Data>0,0,2048,4095" CURVE_ZEROS "</V_Table_Data>\n"
    "    <I_Table_Pointer>0,100,1000,4095" CURVE_ZEROS "</I_Table_Pointer>\n"
    "    <I_Table_Data>0,0,3000,4095" CURVE_ZEROS "</I_Table_Data>\n"
    "  </Node-Throttle>\n</XCT>\n";
static const char throttle_node_curves[] =
    "<XCT>\n  <Node-Throttle>\n"
    "    <V_Table_Pointer>0,100,2000,4095" CURVE_ZEROS "</V_Table_Pointer>\n"
    "    <V_Table_Data>0,0,2048,4095" CURVE_ZEROS "</V_Table_Data>\n"
    "    <I_Table_Pointer>0,100,1000,4095" CURVE_ZEROS "</I_Table_Pointer>\n"
    "    <I_Table_Data>0,0,3000,4095" CURVE_ZEROS "</I_Table_Data>\n"
    "  </Node-Throttle>\n</XCT>\n";
static const char throttle_node_linearization[] =
    "<XCT>\n  <Node-Throttle>\n"
    "    <Lin_Table_Pointer>0,40,4054,4095" CURVE_ZEROS "</Lin_Table_Pointer>\n"
    "    <Lin_Table_Data>0,0,4095,4095" CURVE_ZEROS "</Lin_Table_Data>\n"
    "  </Node-Throttle>\n</XCT>\n";

/* Files holding only throttle entries write just those entries. Only the
 * file with the throttle type and HPD clears calibration; a linearization-only
 * file does not reset. */
static int test_throttle_only_config_files_wire_sequences(void)
{
    const uint16_t speed_x[] = { 0, 100, 2000, 4095 }, speed_y[] = { 0, 0, 2048, 4095 };
    const uint16_t torque_x[] = { 0, 100, 1000, 4095 }, torque_y[] = { 0, 0, 3000, 4095 };
    const struct { const char* xml; bool selection, linearization, curves; } cases[] = {
        { throttle_node_config, true, true, true },
        { throttle_node_curves, false, false, true },
        { throttle_node_linearization, false, true, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        writable_device_reset();
        uint8_t expected[VARSET_SIZE];
        memcpy(expected, device.page, sizeof(expected));
        if (cases[i].selection) {
            expected[0xA0] = 4;
            expected[0xA4] = 1;
            memcpy(expected + 0xC0, "EZGO ITS", 8);
        }
        if (cases[i].linearization)
            put_curve(expected, "linearization", linear_x, linear_y, 4);
        if (cases[i].curves) {
            put_curve(expected, "speed", speed_x, speed_y, 4);
            put_curve(expected, "torque", torque_x, torque_y, 4);
        }

        char path[64];
        ASSERT_EQ(make_temp_path(path), 0);
        ASSERT_EQ(write_file(path, cases[i].xml), 0);
        int rc = run_config_load_file(path);
        remove(path);
        ASSERT_EQ(rc, 0);
        ASSERT_MEM_EQ(device.page, expected, sizeof(expected));
        begin_script();
        expect_settings_write(expected);
        if (cases[i].selection)
            expect_backup_clear();
        if (cases[i].selection || cases[i].curves)
            expect_reset();
        if (check_wire(true)) return 1;
    }
    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* Value rules                                                        */
/* ------------------------------------------------------------------ */

static int run_curve_points(char* type, const char* const* points, size_t n, bool bypass)
{
    char text[8][24];
    char* argv[12] = { "curve", "set", type };
    int argc = 3;
    for (size_t i = 0; i < n && i < 8; i++) {
        snprintf(text[i], sizeof(text[i]), "%s", points[i]);
        argv[argc++] = text[i];
    }
    if (bypass) argv[argc++] = "--skip-rules";
    writable_device_reset();
    return run_writable(cmd_curve, argc, argv);
}

static int test_curve_rules_reject_before_open(void)
{
    const struct { char* type; const char* points[4]; size_t n; } bad[] = {
        { "linearization", { "0,10", "50,50", "100,100" }, 3 },   /* Output at rest */
        { "linearization", { "1,0", "50,50", "100,100" }, 3 },    /* First X not 0 */
        { "speed", { "0,0", "50,100" }, 2 },                      /* Short of full scale */
        { "torque", { "0,0", "0.5,10", "100,100" }, 3 },          /* Step under 1 % */
        { "linearization", { "0,0", "50,60", "100,50" }, 3 },     /* Y falls */
        { "field", { "0,10", "0,5", "750,1" }, 3 },               /* Repeated X */
        { "genfield", { "0,10", "375,5" }, 2 },                   /* Short of full scale */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT_EQ(run_curve_points(bad[i].type, bad[i].points, bad[i].n, false), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(device.mutations, 0);
        ASSERT_EQ(run_curve_points(bad[i].type, bad[i].points, bad[i].n, true), 0);
        ASSERT_EQ(device.erases, 1);
    }

    /* A 1 % step at stored precision, trailing zero points, and a falling
     * field-map Y are allowed. */
    const char* steps[] = { "0,0", "1,0", "99,100", "100,100" };
    ASSERT_EQ(run_curve_points("linearization", steps, 4, false), 0);
    const char* field[] = { "0,10", "375,5", "750,1" };
    ASSERT_EQ(run_curve_points("field", field, 3, false), 0);
    ASSERT_EQ(device.erases, 1);
    return 0;
}

static int run_rule_write(const char* a, const char* b, bool bypass)
{
    char text[2][64];
    char* argv[6] = { "write" };
    int argc = 1;
    const char* args[] = { a, b };
    for (size_t i = 0; i < 2 && args[i]; i++) {
        snprintf(text[i], sizeof(text[i]), "%s", args[i]);
        argv[argc++] = text[i];
    }
    if (bypass) argv[argc++] = "--skip-rules";
    return run_writable(cmd_write, argc, argv);
}

static int test_setting_rules_reject_before_writes(void)
{
    /* The simulated controller is rated 400 A: 10 A is 102 counts. */
    const char* bad[] = {
        "N_RollDetect_BrakingCurrent=9.9A", "U1_RollDetect_BrakingCurrent=-2",
        "N_Throttle_Rate=100.5", "U2_Throttle_Rate_Max=101", "Speed_Limit_On=1",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        writable_device_reset();
        ASSERT_EQ(run_rule_write(bad[i], NULL, false), 1);
        ASSERT_EQ(device.mutations, 0);
        writable_device_reset();
        ASSERT_EQ(run_rule_write(bad[i], NULL, true), 0);
        ASSERT_EQ(device.erases, 1);
    }
    const char* good[] = {
        "N_RollDetect_BrakingCurrent=10A", "U2_RollDetect_BrakingCurrent=-2.5",
        "N_Throttle_Rate=100", "U1_Throttle_Rate_Max=100", "Speed_Limit_On=0",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        writable_device_reset();
        ASSERT_EQ(run_rule_write(good[i], NULL, false), 0);
        ASSERT_EQ(device.erases, 1);
    }
    writable_device_reset();
    device.monitor_speed_sensor = true;
    ASSERT_EQ(run_rule_write("Speed_Limit_On=1", NULL, false), 0);

    /* Every item in a batch is checked. */
    writable_device_reset();
    ASSERT_EQ(run_rule_write("N_Throttle_Rate=50", "N_Throttle_Rate_Max=150", false), 1);
    ASSERT_EQ(device.mutations, 0);
    return 0;
}

static int run_write_args(const char* const* args, size_t n, bool bypass)
{
    char text[6][48];
    char* argv[9] = { "write" };
    int argc = 1;
    for (size_t i = 0; i < n && i < 6; i++) {
        snprintf(text[i], sizeof(text[i]), "%s", args[i]);
        argv[argc++] = text[i];
    }
    if (bypass) argv[argc++] = "--skip-rules";
    return run_writable(cmd_write, argc, argv);
}

static void set_volts(uint8_t* page, const char* name, double volts)
{
    const alltrax_var_def* var = alltrax_find_var(name);
    alltrax_encode_var(var, volts, page + (var->address - VARSET_ADDR));
}

static int test_voltage_rules_use_rated_voltage(void)
{
    const struct { uint16_t rated; const char* assign; bool ok; } cases[] = {
        { 48, "AnalogInputs_Threshold=16.0", true },
        { 48, "AnalogInputs_Threshold=15.9", false },
        { 48, "HiBat_Vlim=62.0", true },
        { 48, "HiBat_Vlim=62.1", false },
        { 48, "BMS_Missing_HiBat_Vlim=62.1", false },
        { 72, "LoBat_Vlim=19.9", false },
        { 72, "HiBat_Vlim=90.0", true },
        { 72, "HiBat_Vlim=90.1", false },
        { 36, "LoBat_Vlim=30.0", false },
        { 36, "BMS_Missing_HiBat_Vlim=30.0", false },
    };
    char out[512];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int bypass = 0; bypass <= 1; bypass++) {
            writable_device_reset();
            device.rated_volts = cases[i].rated;
            /* Existing settings that keep the voltage order satisfied. */
            set_volts(device.page, "AnalogInputs_Threshold", 16.0);
            set_volts(device.page, "LoBat_Vlim", 21.0);
            set_volts(device.page, "HiBat_Vlim", 62.0);
            set_volts(device.page, "BMS_Missing_HiBat_Vlim", 60.0);
            char assign[48];
            snprintf(assign, sizeof(assign), "%s", cases[i].assign);
            char* argv[] = { "write", assign, "--skip-rules" };
            int rc = run_cli_output(cmd_write, bypass ? 3 : 2, argv, out, sizeof(out));
            bool ok = cases[i].ok || bypass;
            ASSERT_EQ(rc, ok ? 0 : 1);
            ASSERT_EQ(device.erases, ok ? 1u : 0u);
            if (!ok)
                ASSERT_TRUE(strstr(out, "outside") != NULL || strstr(out, "defined only") != NULL);
        }
    }
    return 0;
}

static int test_voltage_order_rules(void)
{
    const struct { const char* ksi; const char* low; const char* high; const char* bms; bool ok; } cases[] = {
        { "20", "25", "30", "28", true },
        { "20", "21", "22", "22", true },      /* Exact 1 V gaps */
        { "25", "25.5", "30", "28", false },   /* KSI too close to low battery */
        { "25", "25", "30", "28", false },
        { "20", "29.5", "30", "30", false },   /* Low battery too close to high */
        { "20", "30", "30", "30", false },
        { "20", "25", "30", "24", false },     /* BMS-missing below low battery + 1 V */
        { "20", "25", "30", "31", false },     /* BMS-missing above high battery */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char a[4][48];
        snprintf(a[0], sizeof(a[0]), "AnalogInputs_Threshold=%s", cases[i].ksi);
        snprintf(a[1], sizeof(a[1]), "LoBat_Vlim=%s", cases[i].low);
        snprintf(a[2], sizeof(a[2]), "HiBat_Vlim=%s", cases[i].high);
        snprintf(a[3], sizeof(a[3]), "BMS_Missing_HiBat_Vlim=%s", cases[i].bms);
        const char* args[] = { a[0], a[1], a[2], a[3] };
        writable_device_reset();
        ASSERT_EQ(run_write_args(args, 4, false), cases[i].ok ? 0 : 1);
        ASSERT_EQ(device.mutations ? 1 : 0, cases[i].ok ? 1 : 0);
        writable_device_reset();
        ASSERT_EQ(run_write_args(args, 4, true), 0);
    }

    /* Settings outside the batch come from the user page (KSI 18 V). */
    const char* close[] = { "LoBat_Vlim=18.5" };
    const char* gap[] = { "LoBat_Vlim=19" };
    writable_device_reset();
    ASSERT_EQ(run_write_args(close, 1, false), 1);
    ASSERT_EQ(device.mutations, 0);
    writable_device_reset();
    ASSERT_EQ(run_write_args(gap, 1, false), 0);

    /* While the user page is invalid they come from the factory page, which
     * the write restores first. */
    const char* low[] = { "LoBat_Vlim=28" };
    for (int bad = 0; bad <= 1; bad++) {
        writable_device_reset();
        memcpy(device.factory, device.page, VARSET_SIZE);
        set_volts(device.factory, "BMS_Missing_HiBat_Vlim", bad ? 25 : 55);
        device.page[0] = device.page[1] = 0xFF;
        ASSERT_EQ(run_write_args(low, 1, false), bad ? 1 : 0);
        ASSERT_EQ(device.erases, bad ? 0u : 2u);
    }
    return 0;
}

static int test_throttle_type_rules_use_hardware_mask(void)
{
    const char* unsupported[] = { "Throttle_Type=2" };
    const char* supported[] = { "Throttle_Type=1" };
    const char* none[] = { "Throttle_Type=0" };
    const char* const* cases[] = { unsupported, supported, none };
    for (size_t i = 0; i < 3; i++) {
        for (int bypass = 0; bypass <= 1; bypass++) {
            writable_device_reset();
            device.filter_throttles = true;
            device.throttle_mask = 0x001;  /* Type 1 only */
            bool ok = i != 0 || bypass;
            ASSERT_EQ(run_write_args(cases[i], 1, bypass), ok ? 0 : 1);
            ASSERT_EQ(device.erases, ok ? 1u : 0u);
        }
    }
    return 0;
}

#ifdef ALLTRAX_HAVE_XML
static int test_config_load_applies_rules(void)
{
    static const char* const files[] = {
        "<XCT>\n  <Node-Throttle>\n"
        "    <Lin_Table_Pointer>0,40,4054,4095,0,0,0,0,0,0,0,0,0,0,0,0</Lin_Table_Pointer>\n"
        "    <Lin_Table_Data>100,100,4095,4095,0,0,0,0,0,0,0,0,0,0,0,0</Lin_Table_Data>\n"
        "  </Node-Throttle>\n</XCT>\n",
        "<XCT>\n  <Node-Normal_User>\n"
        "    <N_RollDetect_BrakingCurrent>-50</N_RollDetect_BrakingCurrent>\n"
        "  </Node-Normal_User>\n</XCT>\n",
        "<XCT>\n  <Node-Voltage>\n"
        "    <LoBat_Vlim>700</LoBat_Vlim>\n"
        "  </Node-Voltage>\n</XCT>\n",
        "<XCT>\n  <Node-Throttle>\n"
        "    <Throttle_Type>2</Throttle_Type>\n"
        "  </Node-Throttle>\n</XCT>\n",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[64];
        ASSERT_EQ(make_temp_path(path), 0);
        ASSERT_EQ(write_file(path, files[i]), 0);
        writable_device_reset();
        device.filter_throttles = true;
        device.throttle_mask = 0x001;
        char* checked[] = { "config", "load", "--key", "none", path, "--throttle" };
        int refused = run_writable(cmd_config, 6, checked);
        unsigned mutations = device.mutations;
        writable_device_reset();
        char* bypass[] = { "config", "load", "--key", "none", path, "--throttle",
                           "--skip-rules" };
        int bypassed = run_writable(cmd_config, 7, bypass);
        remove(path);
        ASSERT_EQ(refused, 1);
        ASSERT_EQ(mutations, 0);
        ASSERT_EQ(bypassed, 0);
        ASSERT_EQ(device.erases, 1);
    }
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
    RUN_TEST(test_settings_writes_send_one_reset_after_success);
    RUN_TEST(test_settings_writes_send_no_reset_after_failure);
    RUN_TEST(test_linearization_and_field_map_curves_do_not_reset);
    RUN_TEST(test_get_prints_full_integer_range);
    RUN_TEST(test_get_keeps_scaled_formatting);
    RUN_TEST(test_get_profile_current_shows_amps_and_signed_percent);
    RUN_TEST(test_write_reports_field_map_date_stamp);
    RUN_TEST(test_get_rejects_unknown_names_before_open);
    RUN_TEST(test_debug_read_rejects_bad_arguments_before_open);
    RUN_TEST(test_debug_read_prints_bytes_and_value);
    RUN_TEST(test_debug_read_watch_repeats_until_stopped);
    RUN_TEST(test_get_lists_settings_without_factory_copies);
    RUN_TEST(test_get_reports_goodset_and_reads_factory);
    RUN_TEST(test_fan_test_requires_keyswitch_and_writes_once);
    RUN_TEST(test_clear_autocal_sends_one_reset_after_clear);
    RUN_TEST(test_reset_wait_timeout_does_not_resend_reset);
    RUN_TEST(test_restore_defaults_cli_restores_everything);
    RUN_TEST(test_throttle_list_filters_by_model_and_mask);
    RUN_TEST(test_field_map_history_command);
    RUN_TEST(test_monitor_display_gates_live_samples);
    RUN_TEST(test_monitor_throttle_reads_three_blocks_and_blanks_with_ksi_off);
    RUN_TEST(test_monitor_csv_appends_toolkit_layout_with_timestamp);
    RUN_TEST(test_monitor_rejects_bad_options_before_open);
    RUN_TEST(test_monitor_sets_up_log_before_polling);
    RUN_TEST(test_throttle_list_marks_current_entry);
    RUN_TEST(test_throttle_set_refuses_ambiguous_unknown_and_unavailable);
    RUN_TEST(test_throttle_set_curve_and_label_in_one_batch);
    RUN_TEST(test_throttle_set_after_factory_recovery);
    RUN_TEST(test_throttle_set_uses_type_specific_preset);
    RUN_TEST(test_throttle_set_absolute_installs_linear_curve);
    RUN_TEST(test_throttle_set_disables_hpd_for_pump_and_usb);
    RUN_TEST(test_setting_write_wire_sequence);
    RUN_TEST(test_reset_trigger_write_wire_sequence);
    RUN_TEST(test_throttle_select_wire_sequences);
    RUN_TEST(test_throttle_reselect_installs_default_curve_wire_sequence);
    RUN_TEST(test_restore_defaults_wire_sequence);
    RUN_TEST(test_monitor_user_inputs_read_block_and_bench_values);
    RUN_TEST(test_monitor_throttle_view_read_blocks);
    RUN_TEST(test_curve_rules_reject_before_open);
    RUN_TEST(test_setting_rules_reject_before_writes);
    RUN_TEST(test_voltage_rules_use_rated_voltage);
    RUN_TEST(test_voltage_order_rules);
    RUN_TEST(test_throttle_type_rules_use_hardware_mask);
    RUN_TEST(test_named_writes_need_no_confirmation);
    RUN_TEST(test_curve_reset_all_requires_both_confirmations);
    RUN_TEST(test_field_map_writes_include_current_name);
    RUN_TEST(test_field_map_recovery_uses_factory_name);
    RUN_TEST(test_write_amp_suffix);
    RUN_TEST(test_options_accepted_before_and_after_positionals);
    RUN_TEST(test_invalid_options_and_argument_counts_rejected);
    RUN_TEST(test_write_options_map_to_library_options);
    RUN_TEST(test_bad_command_options_rejected_before_open);
    RUN_TEST(test_fw_flag_reaches_write_options_in_any_position);
#ifdef ALLTRAX_HAVE_XML
    RUN_TEST(test_config_curves_rejected_before_any_settings_write);
    RUN_TEST(test_valid_curve_config_reaches_open);
    RUN_TEST(test_invalid_throttle_config_rejected_before_transport);
    RUN_TEST(test_config_save_includes_every_group_and_curve);
    RUN_TEST(test_config_save_any_read_failure_preserves_destination);
    RUN_TEST(test_config_save_rejects_invalid_settings);
    RUN_TEST(test_config_save_on_unreadable_firmware_preserves_destination);
#ifdef ALLTRAX_HAVE_CRYPTO
    RUN_TEST(test_config_save_encrypts_before_writing);
    RUN_TEST(test_config_key_precedence);
    RUN_TEST(test_config_key_rejects_bad_values_before_open);
    RUN_TEST(test_file_keys_use_separate_environment_variables);
    RUN_TEST(test_config_key_load_round_trip);
#else
    RUN_TEST(test_config_requires_plain_xml_without_crypto);
#endif
    RUN_TEST(test_config_save_write_failure_removes_partial_file);
    RUN_TEST(test_config_save_write_failure_keeps_device_paths);
    RUN_TEST(test_config_round_trip_restores_strings_in_one_erase);
    RUN_TEST(test_config_load_rejects_invalid_strings_before_open);
    RUN_TEST(test_config_load_sends_one_reset_after_success);
    RUN_TEST(test_config_load_requires_group_confirmations);
    RUN_TEST(test_config_load_fails_closed_on_bad_values);
    RUN_TEST(test_config_save_writes_only_settings_in_toolkit_nodes);
    RUN_TEST(test_config_load_wire_sequence);
    RUN_TEST(test_throttle_only_config_files_wire_sequences);
    RUN_TEST(test_config_load_applies_rules);
    RUN_TEST(test_field_map_list_marks_current_map_and_upgrade);
    RUN_TEST(test_field_map_file_errors_stop_before_open);
    RUN_TEST(test_field_map_upgrade_selects_newer_version);
    RUN_TEST(test_field_map_install_writes_named_map_and_pole_count);
#ifdef ALLTRAX_HAVE_CRYPTO
    RUN_TEST(test_fieldmap_key_decrypts_map_file);
#endif
#endif
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
