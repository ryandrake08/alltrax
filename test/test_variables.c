/*
 * test_variables.c — Variable encode/decode, find, and scaling tests
 */

#include "helpers.h"
#include "alltrax.h"

/* ------------------------------------------------------------------ */
/* 1. Variable lookup                                                  */
/* ------------------------------------------------------------------ */

static int test_find_var_lobat(void)
{
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    ASSERT_NOT_NULL(var);
    ASSERT_STR_EQ(var->name, "LoBat_Vlim");
    ASSERT_EQ(var->address, 0x08002020);
    ASSERT_EQ(var->type, ALLTRAX_TYPE_UINT16);
    ASSERT_TRUE(var->writable);
    ASSERT_TRUE(var->is_flash);
    return 0;
}

static int test_find_var_hibat(void)
{
    const alltrax_var_def* var = alltrax_find_var("HiBat_Vlim");
    ASSERT_NOT_NULL(var);
    ASSERT_EQ(var->address, 0x08002022);
    return 0;
}

static int test_find_var_throttle_type(void)
{
    const alltrax_var_def* var = alltrax_find_var("Throttle_Type");
    ASSERT_NOT_NULL(var);
    ASSERT_EQ(var->address, 0x080020A0);
    ASSERT_EQ(var->type, ALLTRAX_TYPE_UINT8);
    return 0;
}

static int test_find_var_nonexistent(void)
{
    const alltrax_var_def* var = alltrax_find_var("nonexistent_var");
    ASSERT_NULL(var);
    return 0;
}

static int test_find_var_fan_on(void)
{
    const alltrax_var_def* var = alltrax_find_var("Fan_On");
    ASSERT_NOT_NULL(var);
    ASSERT_EQ(var->address, 0x2000F204);
    ASSERT_EQ(var->type, ALLTRAX_TYPE_BOOL);
    ASSERT_TRUE(var->writable);
    ASSERT_FALSE(var->is_flash);
    return 0;
}

static int test_find_var_run_mode(void)
{
    const alltrax_var_def* var = alltrax_find_var("RunMode");
    ASSERT_NOT_NULL(var);
    ASSERT_EQ(var->address, 0x2000FFFA);
    ASSERT_FALSE(var->is_flash);
    return 0;
}

static int test_find_var_throttle_type_name(void)
{
    const alltrax_var_def* var = alltrax_find_var("Throttle_Type_Name");
    ASSERT_NOT_NULL(var);
    ASSERT_EQ(var->address, 0x080020C0);
    ASSERT_EQ(var->type, ALLTRAX_TYPE_STRING);
    ASSERT_EQ(var->size, 16);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 2. Group retrieval                                                  */
/* ------------------------------------------------------------------ */

static int test_get_voltage_group(void)
{
    size_t count;
    const alltrax_var_def* vars = alltrax_get_var_defs(ALLTRAX_VARS_VOLTAGE, &count);
    ASSERT_NOT_NULL(vars);
    ASSERT_EQ(count, 4);
    ASSERT_STR_EQ(vars[0].name, "AnalogInputs_Threshold");
    return 0;
}

static int test_get_flags_group(void)
{
    size_t count;
    const alltrax_var_def* vars = alltrax_get_var_defs(ALLTRAX_VARS_FLAGS, &count);
    ASSERT_NOT_NULL(vars);
    ASSERT_EQ(count, 17);
    ASSERT_STR_EQ(vars[0].name, "Global_Shutdown");
    ASSERT_STR_EQ(vars[16].name, "Bad_Vars");
    return 0;
}

static int test_get_write_values_group(void)
{
    size_t count;
    const alltrax_var_def* vars = alltrax_get_var_defs(ALLTRAX_VARS_WRITE_VALUES, &count);
    ASSERT_NOT_NULL(vars);
    ASSERT_EQ(count, 2);
    return 0;
}

static int test_get_invalid_group(void)
{
    size_t count;
    const alltrax_var_def* vars = alltrax_get_var_defs(
        (alltrax_var_group)99, &count);
    ASSERT_NULL(vars);
    ASSERT_EQ(count, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 3. Variable byte sizes                                              */
/* ------------------------------------------------------------------ */

static int test_var_byte_size_bool(void)
{
    const alltrax_var_def* var = alltrax_find_var("Fan_On");
    ASSERT_EQ(alltrax_var_byte_size(var), 1);
    return 0;
}

static int test_var_byte_size_uint8(void)
{
    const alltrax_var_def* var = alltrax_find_var("Throttle_Type");
    ASSERT_EQ(alltrax_var_byte_size(var), 1);
    return 0;
}

static int test_var_byte_size_uint16(void)
{
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    ASSERT_EQ(alltrax_var_byte_size(var), 2);
    return 0;
}

static int test_var_byte_size_int16(void)
{
    const alltrax_var_def* var = alltrax_find_var("Avg_Temp");
    ASSERT_EQ(alltrax_var_byte_size(var), 2);
    return 0;
}

static int test_var_byte_size_int32(void)
{
    const alltrax_var_def* var = alltrax_find_var("Output_Amps");
    ASSERT_EQ(alltrax_var_byte_size(var), 4);
    return 0;
}

static int test_var_byte_size_string(void)
{
    const alltrax_var_def* var = alltrax_find_var("Throttle_Type_Name");
    ASSERT_EQ(alltrax_var_byte_size(var), 16);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 4. Decode — voltage scale (raw 280 * 0.1 = 28.0V)                  */
/* ------------------------------------------------------------------ */

static int test_decode_voltage(void)
{
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    /* Raw value 280 in little-endian uint16 at the var's address */
    uint8_t data[2] = { 0x18, 0x01 };  /* 280 = 0x0118 */
    alltrax_var_value val;

    alltrax_error err = alltrax_decode_var(data, 2, var, var->address, &val);
    ASSERT_EQ(err, ALLTRAX_OK);
    ASSERT_EQ(val.raw.u16, 280);
    ASSERT_NEAR(val.display, 28.0, 0.001);
    return 0;
}

static int test_decode_voltage_620(void)
{
    const alltrax_var_def* var = alltrax_find_var("HiBat_Vlim");
    uint8_t data[2] = { 0x6C, 0x02 };  /* 620 = 0x026C */
    alltrax_var_value val;

    alltrax_decode_var(data, 2, var, var->address, &val);
    ASSERT_EQ(val.raw.u16, 620);
    ASSERT_NEAR(val.display, 62.0, 0.001);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 5. Decode — temperature (raw 628, offset 527, scale 0.1289)         */
/* ------------------------------------------------------------------ */

static int test_decode_temperature(void)
{
    const alltrax_var_def* var = alltrax_find_var("Avg_Temp");
    /* Raw value 628 from capture (0x2000F0E6: 0x7402) */
    uint8_t data[2] = { 0x74, 0x02 };  /* 628 = 0x0274 */
    alltrax_var_value val;

    alltrax_decode_var(data, 2, var, var->address, &val);
    ASSERT_EQ(val.raw.i16, 628);
    /* display = (628 - 527) * 0.1289 = 101 * 0.1289 = 13.0189 */
    ASSERT_NEAR(val.display, 13.0189, 0.01);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 6. Decode — boolean                                                 */
/* ------------------------------------------------------------------ */

static int test_decode_bool_true(void)
{
    const alltrax_var_def* var = alltrax_find_var("Keyswitch_Input");
    uint8_t data[1] = { 0x01 };
    alltrax_var_value val;

    alltrax_decode_var(data, 1, var, var->address, &val);
    ASSERT_TRUE(val.raw.b);
    ASSERT_NEAR(val.display, 1.0, 0.001);
    return 0;
}

static int test_decode_bool_false(void)
{
    const alltrax_var_def* var = alltrax_find_var("Keyswitch_Input");
    uint8_t data[1] = { 0x00 };
    alltrax_var_value val;

    alltrax_decode_var(data, 1, var, var->address, &val);
    ASSERT_FALSE(val.raw.b);
    ASSERT_NEAR(val.display, 0.0, 0.001);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 7. Decode — int32 with scale                                        */
/* ------------------------------------------------------------------ */

static int test_decode_int32_output_amps(void)
{
    const alltrax_var_def* var = alltrax_find_var("Output_Amps");
    /* scale = 0.1. Raw value 1234 → 123.4A */
    uint8_t data[4] = { 0xD2, 0x04, 0x00, 0x00 };  /* 1234 LE */
    alltrax_var_value val;

    alltrax_decode_var(data, 4, var, var->address, &val);
    ASSERT_EQ(val.raw.i32, 1234);
    ASSERT_NEAR(val.display, 123.4, 0.001);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 8. Decode — string                                                  */
/* ------------------------------------------------------------------ */

static int test_decode_string(void)
{
    const alltrax_var_def* var = alltrax_find_var("Model");
    uint8_t data[15];
    memcpy(data, "XCT48400-DCS   ", 15);
    alltrax_var_value val;

    alltrax_decode_var(data, 15, var, var->address, &val);
    ASSERT_STR_EQ(val.raw.str, "XCT48400-DCS");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 9. Decode — from block (offset within data buffer)                  */
/* ------------------------------------------------------------------ */

static int test_decode_from_block(void)
{
    /* BPlus_Volts at 0x2000F110, within block starting at 0x2000F110 */
    const alltrax_var_def* var = alltrax_find_var("BPlus_Volts");
    /* Raw value 381 (0x017D) → 38.1V */
    uint8_t data[56];
    memset(data, 0, sizeof(data));
    data[0] = 0x7D;  /* lo byte */
    data[1] = 0x01;  /* hi byte */
    alltrax_var_value val;

    alltrax_decode_var(data, 56, var, 0x2000F110, &val);
    ASSERT_EQ(val.raw.i16, 381);
    ASSERT_NEAR(val.display, 38.1, 0.001);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 10. Encode — round-trip                                             */
/* ------------------------------------------------------------------ */

static int test_encode_voltage(void)
{
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    uint8_t buf[4];
    int len = alltrax_encode_var(var, 28.0, buf);
    ASSERT_EQ(len, 2);
    /* 28.0 / 0.1 = 280 = 0x0118 */
    ASSERT_EQ(buf[0], 0x18);
    ASSERT_EQ(buf[1], 0x01);
    return 0;
}

static int test_encode_voltage_281(void)
{
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    uint8_t buf[4];
    int len = alltrax_encode_var(var, 28.1, buf);
    ASSERT_EQ(len, 2);
    /* 28.1 / 0.1 = 281 = 0x0119 */
    ASSERT_EQ(buf[0], 0x19);
    ASSERT_EQ(buf[1], 0x01);
    return 0;
}

static int test_encode_decode_round_trip(void)
{
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    uint8_t buf[4];
    alltrax_encode_var(var, 28.0, buf);

    alltrax_var_value val;
    alltrax_decode_var(buf, 2, var, var->address, &val);
    ASSERT_NEAR(val.display, 28.0, 0.001);
    return 0;
}

static int test_encode_bool(void)
{
    const alltrax_var_def* var = alltrax_find_var("HPD");
    uint8_t buf[4];
    int len = alltrax_encode_var(var, 1.0, buf);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(buf[0], 1);

    len = alltrax_encode_var(var, 0.0, buf);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(buf[0], 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 11. Error flag names                                                */
/* ------------------------------------------------------------------ */

static int test_error_flag_names(void)
{
    ASSERT_STR_EQ(alltrax_error_flag_name(0), "Global Shutdown");
    ASSERT_STR_EQ(alltrax_error_flag_name(3), "Low Battery");
    ASSERT_STR_EQ(alltrax_error_flag_name(6), "Over Temperature");
    ASSERT_STR_EQ(alltrax_error_flag_name(16), "Bad Vars");
    ASSERT_NULL(alltrax_error_flag_name(17));
    ASSERT_NULL(alltrax_error_flag_name(-1));
    return 0;
}

/* ------------------------------------------------------------------ */
/* 12. Decode out of bounds                                            */
/* ------------------------------------------------------------------ */

static int test_decode_out_of_bounds(void)
{
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    uint8_t data[1] = { 0 };  /* Too small for uint16 */
    alltrax_var_value val;

    alltrax_error err = alltrax_decode_var(data, 1, var, var->address, &val);
    ASSERT_EQ(err, ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test runner                                                         */
/* ------------------------------------------------------------------ */

void run_variables_tests(void)
{
    printf("variables:\n");

    /* Lookup */
    RUN_TEST(test_find_var_lobat);
    RUN_TEST(test_find_var_hibat);
    RUN_TEST(test_find_var_throttle_type);
    RUN_TEST(test_find_var_nonexistent);
    RUN_TEST(test_find_var_fan_on);
    RUN_TEST(test_find_var_run_mode);
    RUN_TEST(test_find_var_throttle_type_name);

    /* Groups */
    RUN_TEST(test_get_voltage_group);
    RUN_TEST(test_get_flags_group);
    RUN_TEST(test_get_write_values_group);
    RUN_TEST(test_get_invalid_group);

    /* Byte sizes */
    RUN_TEST(test_var_byte_size_bool);
    RUN_TEST(test_var_byte_size_uint8);
    RUN_TEST(test_var_byte_size_uint16);
    RUN_TEST(test_var_byte_size_int16);
    RUN_TEST(test_var_byte_size_int32);
    RUN_TEST(test_var_byte_size_string);

    /* Decode */
    RUN_TEST(test_decode_voltage);
    RUN_TEST(test_decode_voltage_620);
    RUN_TEST(test_decode_temperature);
    RUN_TEST(test_decode_bool_true);
    RUN_TEST(test_decode_bool_false);
    RUN_TEST(test_decode_int32_output_amps);
    RUN_TEST(test_decode_string);
    RUN_TEST(test_decode_from_block);

    /* Encode */
    RUN_TEST(test_encode_voltage);
    RUN_TEST(test_encode_voltage_281);
    RUN_TEST(test_encode_decode_round_trip);
    RUN_TEST(test_encode_bool);

    /* Error flag names */
    RUN_TEST(test_error_flag_names);

    /* Edge cases */
    RUN_TEST(test_decode_out_of_bounds);
}
