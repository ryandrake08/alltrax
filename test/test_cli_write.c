/* Write-command preflight tests. No USB transport is linked. */
#include "helpers.h"
#include "internal.h"
#include "../cli/cli.h"

int test_pass_count;
int test_fail_count;
static unsigned open_calls, unexpected_calls;
static bool open_succeeds;
static alltrax_controller fake_ctrl;
static unsigned error_calls, close_calls, transport_calls;
static alltrax_error reported_error;
static char reported_detail[256];

/* These tests pass positional assignments only; flag parsing is out of scope. */
int cli_parse_flags(int argc, char** argv, cli_flags* flags)
{
    (void)argc;
    (void)argv;
    memset(flags, 0, sizeof(*flags));
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

int main(void)
{
    printf("CLI write preflight (no USB):\n");
    RUN_TEST(test_strings_rejected_before_open);
    RUN_TEST(test_mixed_batch_rejected_before_open);
    RUN_TEST(test_numeric_and_bool_assignments_reach_open);
    RUN_TEST(test_read_only_assignments_use_library_diagnostics);
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
