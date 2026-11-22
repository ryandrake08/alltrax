/* Write-command preflight tests. No USB transport is linked. */
#include "helpers.h"
#include "../cli/cli.h"

int test_pass_count;
int test_fail_count;
static unsigned open_calls, unexpected_calls;

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
    (void)ctrl;
    (void)writes;
    open_calls++;
    return ALLTRAX_ERR_INVALID_ARG;
}

void cli_error(alltrax_controller* ctrl, alltrax_error err, const char* action)
{
    (void)ctrl;
    (void)err;
    (void)action;
    unexpected_calls++;
}

void alltrax_close(alltrax_controller* ctrl)
{
    (void)ctrl;
    unexpected_calls++;
}

alltrax_error alltrax_write_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
    const alltrax_write_opts* opts)
{
    (void)ctrl;
    (void)vars;
    (void)values;
    (void)count;
    (void)opts;
    unexpected_calls++;
    return ALLTRAX_ERR_INVALID_ARG;
}

alltrax_error alltrax_reset_device(alltrax_controller* ctrl)
{
    (void)ctrl;
    unexpected_calls++;
    return ALLTRAX_ERR_INVALID_ARG;
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

int main(void)
{
    printf("CLI write preflight (no USB):\n");
    RUN_TEST(test_strings_rejected_before_open);
    RUN_TEST(test_mixed_batch_rejected_before_open);
    RUN_TEST(test_numeric_and_bool_assignments_reach_open);
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
