/* CLI mutation preflight tests. No USB transport is linked. */
#define _POSIX_C_SOURCE 200809L
#include "helpers.h"
#include "internal.h"
#include "../cli/cli.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

int test_pass_count;
int test_fail_count;
static unsigned open_calls, unexpected_calls;
static bool open_succeeds;
static alltrax_controller fake_ctrl;
static unsigned error_calls, close_calls, transport_calls;
static alltrax_error reported_error;
static char reported_detail[256];

/* Only config's leading --no-crypt flag is needed here; general flag parsing
 * is out of scope. */
int cli_parse_flags(int argc, char** argv, cli_flags* flags)
{
    memset(flags, 0, sizeof(*flags));
    if (argc > 1 && strcmp(argv[1], "--no-crypt") == 0) {
        flags->no_crypt = true;
        return 2;
    }
    return 1;
}

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

/* Use the real controller implementation; any transport call fails the test. */
alltrax_error transport_write(alltrax_controller* ctrl,
    const uint8_t data[PACKET_SIZE])
{
    (void)ctrl;
    (void)data;
    transport_calls++;
    return ALLTRAX_ERR_USB;
}

alltrax_error transport_read(alltrax_controller* ctrl, uint8_t buf[PACKET_SIZE])
{
    (void)ctrl;
    (void)buf;
    transport_calls++;
    return ALLTRAX_ERR_USB;
}

static int test_strings_rejected_before_open(void)
{
    const char* names[] = { "Throttle_Type_Name", "F_Table_Name" };
    const char* values[] = { "123", "", "Custom name" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
            char arg[80];
            snprintf(arg, sizeof(arg), "%s=%s", names[i], values[j]);
            char* argv[] = { "write", arg };
            open_calls = unexpected_calls = 0;
            ASSERT_EQ(cmd_write(2, argv), 1);
            ASSERT_EQ(open_calls, 0);
            ASSERT_EQ(unexpected_calls, 0);
        }
    }
    return 0;
}

static int test_mixed_batch_rejected_before_open(void)
{
    for (int pos = 0; pos < 2; pos++) {
        char numeric[] = "HPD=1";
        char string[] = "F_Table_Name=123";
        char* argv[] = { "write", pos ? numeric : string, pos ? string : numeric };
        open_calls = unexpected_calls = 0;
        ASSERT_EQ(cmd_write(3, argv), 1);
        ASSERT_EQ(open_calls, 0);
        ASSERT_EQ(unexpected_calls, 0);
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

int main(void)
{
    printf("CLI mutation preflight (no USB):\n");
    RUN_TEST(test_strings_rejected_before_open);
    RUN_TEST(test_mixed_batch_rejected_before_open);
    RUN_TEST(test_numeric_and_bool_assignments_reach_open);
    RUN_TEST(test_read_only_assignments_use_library_diagnostics);
    RUN_TEST(test_invalid_numeric_text_rejected_before_open);
    RUN_TEST(test_throttle_enum_assignments_use_shared_validation);
    RUN_TEST(test_invalid_curve_arguments_rejected_before_open);
    RUN_TEST(test_curve_csv_validation);
    RUN_TEST(test_curve_presets_reach_open);
#ifdef TEST_CONFIG
    RUN_TEST(test_config_curves_rejected_before_any_settings_write);
    RUN_TEST(test_valid_curve_config_reaches_open);
    RUN_TEST(test_invalid_throttle_config_rejected_before_transport);
#endif
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
