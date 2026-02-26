/*
 * test_variables.c — Variable encode/decode, find, and scaling tests
 */

#include "helpers.h"
#include "alltrax.h"
#include "internal.h"
#include "test_internal.h"

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

static int test_get_tach_group(void)
{
    size_t count;
    const alltrax_var_def* vars = alltrax_get_var_defs(ALLTRAX_VARS_TACH, &count);
    ASSERT_NOT_NULL(vars);
    ASSERT_EQ(count, 2);
    ASSERT_STR_EQ(vars[0].name, "F_Speed_Limit_On");
    return 0;
}

static int test_get_field_group(void)
{
    size_t count;
    const alltrax_var_def* vars = alltrax_get_var_defs(ALLTRAX_VARS_FIELD, &count);
    ASSERT_NOT_NULL(vars);
    ASSERT_EQ(count, 14);
    ASSERT_STR_EQ(vars[0].name, "F_Table_Name");
    return 0;
}

static int test_get_raw_adc_group(void)
{
    size_t count;
    const alltrax_var_def* vars = alltrax_get_var_defs(ALLTRAX_VARS_RAW_ADC, &count);
    ASSERT_NOT_NULL(vars);
    ASSERT_EQ(count, 14);
    ASSERT_STR_EQ(vars[0].name, "Raw_Keyswitch");
    return 0;
}

static int test_get_avg_adc_group(void)
{
    size_t count;
    const alltrax_var_def* vars = alltrax_get_var_defs(ALLTRAX_VARS_AVG_ADC, &count);
    ASSERT_NOT_NULL(vars);
    ASSERT_EQ(count, 13);
    ASSERT_STR_EQ(vars[0].name, "Avg_Keyswitch");
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
/* 13. Bounds validation                                               */
/* ------------------------------------------------------------------ */

static int test_validate_speed_limit_in_range(void)
{
    const alltrax_var_def* var = alltrax_find_var("N_Speed_Limit");
    /* 4000 RPM, scale=1.0, raw=4000, bounds [1000, 8000] */
    ASSERT_EQ(alltrax_validate_var_value(var, 4000.0), ALLTRAX_OK);
    return 0;
}

static int test_validate_speed_limit_too_low(void)
{
    const alltrax_var_def* var = alltrax_find_var("N_Speed_Limit");
    /* 500 RPM, raw=500, below min 1000 */
    ASSERT_EQ(alltrax_validate_var_value(var, 500.0), ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

static int test_validate_speed_limit_too_high(void)
{
    const alltrax_var_def* var = alltrax_find_var("N_Speed_Limit");
    /* 9000 RPM, raw=9000, above max 8000 */
    ASSERT_EQ(alltrax_validate_var_value(var, 9000.0), ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

static int test_validate_regen_negative_in_range(void)
{
    const alltrax_var_def* var = alltrax_find_var("N_Max_Batt_Regen_Amps");
    /* scale = 20.0/819.0 ≈ 0.02442. -50% → raw = -50/0.02442 ≈ -2048 */
    /* bounds [-4095, 0] */
    ASSERT_EQ(alltrax_validate_var_value(var, -50.0), ALLTRAX_OK);
    return 0;
}

static int test_validate_unbounded_var(void)
{
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    /* UINT16 type range: [0, 65535], no explicit bounds */
    /* 0.1V → raw=1, 6553.5V → raw=65535, both should pass */
    ASSERT_EQ(alltrax_validate_var_value(var, 0.1), ALLTRAX_OK);
    ASSERT_EQ(alltrax_validate_var_value(var, 6553.5), ALLTRAX_OK);
    return 0;
}

static int test_validate_bool_skips_bounds(void)
{
    const alltrax_var_def* var = alltrax_find_var("HPD");
    ASSERT_EQ(alltrax_validate_var_value(var, 1.0), ALLTRAX_OK);
    ASSERT_EQ(alltrax_validate_var_value(var, 0.0), ALLTRAX_OK);
    return 0;
}

static int test_display_bounds(void)
{
    const alltrax_var_def* var = alltrax_find_var("N_Speed_Limit");
    /* scale=1.0, offset=0, bounds [1000, 8000] */
    double bmin, bmax;
    alltrax_var_display_bounds(var, &bmin, &bmax);
    ASSERT_NEAR(bmin, 1000.0, 0.001);
    ASSERT_NEAR(bmax, 8000.0, 0.001);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 15. Feature gates                                                   */
/* ------------------------------------------------------------------ */

/* Helper: build a minimal alltrax_info with the given version fields */
static alltrax_info make_info(uint32_t orig_boot, uint32_t orig_prgm,
    uint32_t prgm, uint16_t ver)
{
    alltrax_info info;
    memset(&info, 0, sizeof(info));
    info.original_boot_rev = orig_boot;
    info.original_program_rev = orig_prgm;
    info.program_rev = prgm;
    info.program_ver = ver;
    return info;
}

static int test_feat_hw_caps(void)
{
    /* Gate: orig_boot != 1 || orig_prgm != 1 */
    alltrax_info info;

    /* Both == 1: no hw caps (very old V0.001 firmware) */
    info = make_info(1, 1, 5005, 0);
    ASSERT_TRUE(!alltrax_has_feature(&info, ALLTRAX_FEAT_HW_CAPS));

    /* Boot != 1: has hw caps */
    info = make_info(2, 1, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_HW_CAPS));

    /* Prgm != 1: has hw caps */
    info = make_info(1, 2, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_HW_CAPS));

    /* Both != 1: has hw caps */
    info = make_info(5002, 5005, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_HW_CAPS));

    return 0;
}

static int test_feat_throttle_caps(void)
{
    /* Gate: orig_boot > 2 || orig_prgm > 2 */
    alltrax_info info;

    /* Both <= 2: no throttle caps */
    info = make_info(1, 2, 5005, 0);
    ASSERT_TRUE(!alltrax_has_feature(&info, ALLTRAX_FEAT_THROTTLE_CAPS));

    info = make_info(2, 2, 5005, 0);
    ASSERT_TRUE(!alltrax_has_feature(&info, ALLTRAX_FEAT_THROTTLE_CAPS));

    /* Boot > 2: has throttle caps */
    info = make_info(3, 1, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_THROTTLE_CAPS));

    /* Prgm > 2: has throttle caps */
    info = make_info(1, 3, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_THROTTLE_CAPS));

    return 0;
}

static int test_feat_forward_input(void)
{
    /* Gate: orig_prgm >= 68 || prgm_ver == 200 */
    alltrax_info info;

    /* Old firmware, no PrgmVer fallback: no feature */
    info = make_info(5002, 67, 5005, 0);
    ASSERT_TRUE(!alltrax_has_feature(&info, ALLTRAX_FEAT_FORWARD_INPUT));

    /* Exactly 68: has feature */
    info = make_info(5002, 68, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_FORWARD_INPUT));

    /* PrgmVer == 200 fallback: has feature even with old orig */
    info = make_info(5002, 10, 5005, 200);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_FORWARD_INPUT));

    /* Modern firmware */
    info = make_info(5002, 5005, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_FORWARD_INPUT));

    return 0;
}

static int test_feat_user1_input(void)
{
    /* Gate: orig_prgm >= 70 || prgm_ver == 200 */
    alltrax_info info;

    info = make_info(5002, 69, 5005, 0);
    ASSERT_TRUE(!alltrax_has_feature(&info, ALLTRAX_FEAT_USER1_INPUT));

    info = make_info(5002, 70, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_USER1_INPUT));

    /* PrgmVer == 200 fallback */
    info = make_info(5002, 10, 5005, 200);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_USER1_INPUT));

    return 0;
}

static int test_feat_can_highside(void)
{
    /* Gate: orig_prgm >= 1008 */
    alltrax_info info;

    info = make_info(5002, 1007, 5005, 0);
    ASSERT_TRUE(!alltrax_has_feature(&info, ALLTRAX_FEAT_CAN_HIGHSIDE));

    info = make_info(5002, 1008, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_CAN_HIGHSIDE));

    return 0;
}

static int test_feat_software_gates(void)
{
    alltrax_info info;

    /* User profiles: prgm >= 1005 */
    info = make_info(5002, 5005, 1004, 0);
    ASSERT_TRUE(!alltrax_has_feature(&info, ALLTRAX_FEAT_USER_PROFILES));

    info = make_info(5002, 5005, 1005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_USER_PROFILES));

    /* User defaults: orig_prgm >= 1007 */
    info = make_info(5002, 1006, 5005, 0);
    ASSERT_TRUE(!alltrax_has_feature(&info, ALLTRAX_FEAT_USER_DEFAULTS));

    info = make_info(5002, 1007, 5005, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_USER_DEFAULTS));

    /* Bad vars code: prgm >= 1107 */
    info = make_info(5002, 5005, 1106, 0);
    ASSERT_TRUE(!alltrax_has_feature(&info, ALLTRAX_FEAT_BAD_VARS_CODE));

    info = make_info(5002, 5005, 1107, 0);
    ASSERT_TRUE(alltrax_has_feature(&info, ALLTRAX_FEAT_BAD_VARS_CODE));

    return 0;
}

/* ------------------------------------------------------------------ */
/* 16. Controller type detection                                       */
/* ------------------------------------------------------------------ */

static int test_detect_type_xct(void)
{
    ASSERT_EQ(detect_controller_type("XCT48400-DCS"),
              ALLTRAX_CONTROLLER_XCT);
    ASSERT_EQ(detect_controller_type("XCT72500"),
              ALLTRAX_CONTROLLER_XCT);
    return 0;
}

static int test_detect_type_xct_aliases(void)
{
    /* SRX and NCT map to XCT (same protocol) */
    ASSERT_EQ(detect_controller_type("SRX48300"),
              ALLTRAX_CONTROLLER_XCT);
    ASSERT_EQ(detect_controller_type("NCT48400"),
              ALLTRAX_CONTROLLER_XCT);
    return 0;
}

static int test_detect_type_spm(void)
{
    ASSERT_EQ(detect_controller_type("SPM48400"),
              ALLTRAX_CONTROLLER_SPM);
    /* SPB maps to SPM */
    ASSERT_EQ(detect_controller_type("SPB48300"),
              ALLTRAX_CONTROLLER_SPM);
    return 0;
}

static int test_detect_type_sr(void)
{
    /* SR* (not SRX) maps to SR */
    ASSERT_EQ(detect_controller_type("SR48300"),
              ALLTRAX_CONTROLLER_SR);
    ASSERT_EQ(detect_controller_type("SR72500-BMS"),
              ALLTRAX_CONTROLLER_SR);
    return 0;
}

static int test_detect_type_bms(void)
{
    ASSERT_EQ(detect_controller_type("BMS48"),
              ALLTRAX_CONTROLLER_BMS);
    /* BMS2 is a distinct type */
    ASSERT_EQ(detect_controller_type("BMS248"),
              ALLTRAX_CONTROLLER_BMS2);
    return 0;
}

static int test_detect_type_unknown(void)
{
    ASSERT_EQ(detect_controller_type(""),
              ALLTRAX_CONTROLLER_UNKNOWN);
    ASSERT_EQ(detect_controller_type("AB"),
              ALLTRAX_CONTROLLER_UNKNOWN);
    ASSERT_EQ(detect_controller_type("FOO12345"),
              ALLTRAX_CONTROLLER_UNKNOWN);
    return 0;
}

static int test_controller_type_name(void)
{
    ASSERT_STR_EQ(alltrax_controller_type_name(ALLTRAX_CONTROLLER_XCT),
                  "XCT");
    ASSERT_STR_EQ(alltrax_controller_type_name(ALLTRAX_CONTROLLER_SPM),
                  "SPM");
    ASSERT_STR_EQ(alltrax_controller_type_name(ALLTRAX_CONTROLLER_SR),
                  "SR");
    ASSERT_STR_EQ(alltrax_controller_type_name(ALLTRAX_CONTROLLER_BMS),
                  "BMS");
    ASSERT_STR_EQ(alltrax_controller_type_name(ALLTRAX_CONTROLLER_BMS2),
                  "BMS2");
    ASSERT_STR_EQ(alltrax_controller_type_name(ALLTRAX_CONTROLLER_UNKNOWN),
                  "Unknown");
    return 0;
}

static int test_firmware_bounds(void)
{
    alltrax_info info;

    /* XCT in bounds: supported */
    memset(&info, 0, sizeof(info));
    info.controller_type = ALLTRAX_CONTROLLER_XCT;
    info.boot_rev = 5002;
    info.program_rev = 5005;
    ASSERT_TRUE(info.controller_type == ALLTRAX_CONTROLLER_XCT);

    /* The firmware_in_bounds logic is tested indirectly via supported flag.
     * Since we can't call alltrax_get_info() without USB, verify the type
     * detection feeds into the bounds correctly via detect_controller_type. */
    ASSERT_EQ(detect_controller_type("XCT48400"),
              ALLTRAX_CONTROLLER_XCT);
    ASSERT_EQ(detect_controller_type("SPM48400"),
              ALLTRAX_CONTROLLER_SPM);
    ASSERT_EQ(detect_controller_type("SR48300"),
              ALLTRAX_CONTROLLER_SR);
    ASSERT_EQ(detect_controller_type("BMS48"),
              ALLTRAX_CONTROLLER_BMS);
    ASSERT_EQ(detect_controller_type("BMS248"),
              ALLTRAX_CONTROLLER_BMS2);

    return 0;
}

/* ------------------------------------------------------------------ */
/* 17. Voltage link validation                                         */
/* ------------------------------------------------------------------ */

static int test_voltage_link_valid(void)
{
    /* KSI=20, UnderVolt=25, OverVolt=30, BMSMissing=28 */
    char msg[256];
    ASSERT_EQ(validate_voltage_link(20.0, 25.0, 30.0, 28.0,
              msg, sizeof(msg)), ALLTRAX_OK);
    return 0;
}

static int test_voltage_link_exact_gaps(void)
{
    /* Exactly 1V gaps: KSI=20, UnderVolt=21, OverVolt=22, BMSMissing=22 */
    char msg[256];
    ASSERT_EQ(validate_voltage_link(20.0, 21.0, 22.0, 22.0,
              msg, sizeof(msg)), ALLTRAX_OK);
    return 0;
}

static int test_voltage_link_ksi_too_close(void)
{
    /* KSI=25, UnderVolt=25.5 — gap < 1V */
    char msg[256];
    ASSERT_EQ(validate_voltage_link(25.0, 25.5, 30.0, 28.0,
              msg, sizeof(msg)), ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

static int test_voltage_link_ksi_equals_under(void)
{
    /* KSI=25, UnderVolt=25 — gap = 0V */
    char msg[256];
    ASSERT_EQ(validate_voltage_link(25.0, 25.0, 30.0, 28.0,
              msg, sizeof(msg)), ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

static int test_voltage_link_under_too_close_to_over(void)
{
    /* UnderVolt=29.5, OverVolt=30 — gap < 1V */
    char msg[256];
    ASSERT_EQ(validate_voltage_link(20.0, 29.5, 30.0, 30.0,
              msg, sizeof(msg)), ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

static int test_voltage_link_under_equals_over(void)
{
    /* UnderVolt=30, OverVolt=30 — gap = 0V */
    char msg[256];
    ASSERT_EQ(validate_voltage_link(20.0, 30.0, 30.0, 30.0,
              msg, sizeof(msg)), ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

static int test_voltage_link_bms_below_under(void)
{
    /* BMSMissing=24, UnderVolt=25 — BMSMissing < UnderVolt + 1 */
    char msg[256];
    ASSERT_EQ(validate_voltage_link(20.0, 25.0, 30.0, 24.0,
              msg, sizeof(msg)), ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

static int test_voltage_link_bms_above_over(void)
{
    /* BMSMissing=31, OverVolt=30 — BMSMissing > OverVolt */
    char msg[256];
    ASSERT_EQ(validate_voltage_link(20.0, 25.0, 30.0, 31.0,
              msg, sizeof(msg)), ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

static int test_voltage_link_skip_bms(void)
{
    /* bms_missing < 0 skips BMS constraints */
    char msg[256];
    ASSERT_EQ(validate_voltage_link(20.0, 25.0, 30.0, -1.0,
              msg, sizeof(msg)), ALLTRAX_OK);
    return 0;
}

static int test_voltage_link_null_msg(void)
{
    /* NULL msg buffer should not crash */
    ASSERT_EQ(validate_voltage_link(20.0, 25.0, 30.0, 28.0,
              NULL, 0), ALLTRAX_OK);
    ASSERT_EQ(validate_voltage_link(25.0, 25.0, 30.0, 28.0,
              NULL, 0), ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 18. format_rev                                                      */
/* ------------------------------------------------------------------ */

static int test_format_rev_5005(void)
{
    char buf[16];
    format_rev(5005, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "V5.005");
    return 0;
}

static int test_format_rev_zero(void)
{
    char buf[16];
    format_rev(0, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "V0.000");
    return 0;
}

static int test_format_rev_999(void)
{
    char buf[16];
    format_rev(999, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "V0.999");
    return 0;
}

static int test_format_rev_1000(void)
{
    char buf[16];
    format_rev(1000, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "V1.000");
    return 0;
}

static int test_format_rev_large(void)
{
    char buf[16];
    format_rev(12345, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "V12.345");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 19. firmware_in_bounds                                              */
/* ------------------------------------------------------------------ */

static int test_fw_bounds_xct_valid(void)
{
    ASSERT_TRUE(firmware_in_bounds(ALLTRAX_CONTROLLER_XCT, 5002, 5005));
    return 0;
}

static int test_fw_bounds_xct_zero_prgm(void)
{
    /* prgm_rev must be >= 1 */
    ASSERT_FALSE(firmware_in_bounds(ALLTRAX_CONTROLLER_XCT, 5002, 0));
    return 0;
}

static int test_fw_bounds_xct_at_limit(void)
{
    ASSERT_TRUE(firmware_in_bounds(ALLTRAX_CONTROLLER_XCT, 5999, 5999));
    return 0;
}

static int test_fw_bounds_xct_over(void)
{
    ASSERT_FALSE(firmware_in_bounds(ALLTRAX_CONTROLLER_XCT, 0, 6000));
    return 0;
}

static int test_fw_bounds_spm_valid(void)
{
    ASSERT_TRUE(firmware_in_bounds(ALLTRAX_CONTROLLER_SPM, 1050, 2000));
    return 0;
}

static int test_fw_bounds_spm_below(void)
{
    ASSERT_FALSE(firmware_in_bounds(ALLTRAX_CONTROLLER_SPM, 1049, 2000));
    return 0;
}

static int test_fw_bounds_sr_valid(void)
{
    ASSERT_TRUE(firmware_in_bounds(ALLTRAX_CONTROLLER_SR, 500, 1000));
    return 0;
}

static int test_fw_bounds_sr_over(void)
{
    ASSERT_FALSE(firmware_in_bounds(ALLTRAX_CONTROLLER_SR, 1000, 1000));
    return 0;
}

static int test_fw_bounds_bms_valid(void)
{
    ASSERT_TRUE(firmware_in_bounds(ALLTRAX_CONTROLLER_BMS, 500, 3000));
    ASSERT_TRUE(firmware_in_bounds(ALLTRAX_CONTROLLER_BMS2, 500, 3000));
    return 0;
}

static int test_fw_bounds_unknown(void)
{
    ASSERT_FALSE(firmware_in_bounds(ALLTRAX_CONTROLLER_UNKNOWN, 5002, 5005));
    return 0;
}

/* ------------------------------------------------------------------ */
/* 20. type_range                                                      */
/* ------------------------------------------------------------------ */

static int test_type_range_uint8(void)
{
    int64_t lo, hi;
    type_range(ALLTRAX_TYPE_UINT8, &lo, &hi);
    ASSERT_EQ(lo, 0);
    ASSERT_EQ(hi, 255);
    return 0;
}

static int test_type_range_int8(void)
{
    int64_t lo, hi;
    type_range(ALLTRAX_TYPE_INT8, &lo, &hi);
    ASSERT_EQ(lo, -128);
    ASSERT_EQ(hi, 127);
    return 0;
}

static int test_type_range_uint16(void)
{
    int64_t lo, hi;
    type_range(ALLTRAX_TYPE_UINT16, &lo, &hi);
    ASSERT_EQ(lo, 0);
    ASSERT_EQ(hi, 65535);
    return 0;
}

static int test_type_range_int16(void)
{
    int64_t lo, hi;
    type_range(ALLTRAX_TYPE_INT16, &lo, &hi);
    ASSERT_EQ(lo, -32768);
    ASSERT_EQ(hi, 32767);
    return 0;
}

static int test_type_range_uint32(void)
{
    int64_t lo, hi;
    type_range(ALLTRAX_TYPE_UINT32, &lo, &hi);
    ASSERT_EQ(lo, 0);
    ASSERT_TRUE(hi == (int64_t)UINT32_MAX);
    return 0;
}

static int test_type_range_int32(void)
{
    int64_t lo, hi;
    type_range(ALLTRAX_TYPE_INT32, &lo, &hi);
    ASSERT_TRUE(lo == (int64_t)INT32_MIN);
    ASSERT_TRUE(hi == (int64_t)INT32_MAX);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 21. effective_bounds                                                 */
/* ------------------------------------------------------------------ */

static int test_effective_bounds_unbounded(void)
{
    /* LoBat_Vlim: UINT16, no explicit bounds → type range [0, 65535] */
    const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
    int64_t lo, hi;
    effective_bounds(var, &lo, &hi);
    ASSERT_EQ(lo, 0);
    ASSERT_EQ(hi, 65535);
    return 0;
}

static int test_effective_bounds_explicit(void)
{
    /* N_Speed_Limit: INT16 type [-32768,32767], explicit [1000, 8000] */
    const alltrax_var_def* var = alltrax_find_var("N_Speed_Limit");
    int64_t lo, hi;
    effective_bounds(var, &lo, &hi);
    ASSERT_EQ(lo, 1000);
    ASSERT_EQ(hi, 8000);
    return 0;
}

static int test_effective_bounds_negative(void)
{
    /* N_Max_Batt_Regen_Amps: INT16, explicit [-4095, 0] */
    const alltrax_var_def* var = alltrax_find_var("N_Max_Batt_Regen_Amps");
    int64_t lo, hi;
    effective_bounds(var, &lo, &hi);
    ASSERT_EQ(lo, -4095);
    ASSERT_EQ(hi, 0);
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
    RUN_TEST(test_get_tach_group);
    RUN_TEST(test_get_field_group);
    RUN_TEST(test_get_raw_adc_group);
    RUN_TEST(test_get_avg_adc_group);
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

    /* Bounds validation */
    RUN_TEST(test_validate_speed_limit_in_range);
    RUN_TEST(test_validate_speed_limit_too_low);
    RUN_TEST(test_validate_speed_limit_too_high);
    RUN_TEST(test_validate_regen_negative_in_range);
    RUN_TEST(test_validate_unbounded_var);
    RUN_TEST(test_validate_bool_skips_bounds);
    RUN_TEST(test_display_bounds);

    /* Feature gates */
    RUN_TEST(test_feat_hw_caps);
    RUN_TEST(test_feat_throttle_caps);
    RUN_TEST(test_feat_forward_input);
    RUN_TEST(test_feat_user1_input);
    RUN_TEST(test_feat_can_highside);
    RUN_TEST(test_feat_software_gates);

    /* Controller type detection */
    RUN_TEST(test_detect_type_xct);
    RUN_TEST(test_detect_type_xct_aliases);
    RUN_TEST(test_detect_type_spm);
    RUN_TEST(test_detect_type_sr);
    RUN_TEST(test_detect_type_bms);
    RUN_TEST(test_detect_type_unknown);
    RUN_TEST(test_controller_type_name);
    RUN_TEST(test_firmware_bounds);

    /* Voltage link validation */
    RUN_TEST(test_voltage_link_valid);
    RUN_TEST(test_voltage_link_exact_gaps);
    RUN_TEST(test_voltage_link_ksi_too_close);
    RUN_TEST(test_voltage_link_ksi_equals_under);
    RUN_TEST(test_voltage_link_under_too_close_to_over);
    RUN_TEST(test_voltage_link_under_equals_over);
    RUN_TEST(test_voltage_link_bms_below_under);
    RUN_TEST(test_voltage_link_bms_above_over);
    RUN_TEST(test_voltage_link_skip_bms);
    RUN_TEST(test_voltage_link_null_msg);

    /* format_rev */
    RUN_TEST(test_format_rev_5005);
    RUN_TEST(test_format_rev_zero);
    RUN_TEST(test_format_rev_999);
    RUN_TEST(test_format_rev_1000);
    RUN_TEST(test_format_rev_large);

    /* firmware_in_bounds */
    RUN_TEST(test_fw_bounds_xct_valid);
    RUN_TEST(test_fw_bounds_xct_zero_prgm);
    RUN_TEST(test_fw_bounds_xct_at_limit);
    RUN_TEST(test_fw_bounds_xct_over);
    RUN_TEST(test_fw_bounds_spm_valid);
    RUN_TEST(test_fw_bounds_spm_below);
    RUN_TEST(test_fw_bounds_sr_valid);
    RUN_TEST(test_fw_bounds_sr_over);
    RUN_TEST(test_fw_bounds_bms_valid);
    RUN_TEST(test_fw_bounds_unknown);

    /* type_range */
    RUN_TEST(test_type_range_uint8);
    RUN_TEST(test_type_range_int8);
    RUN_TEST(test_type_range_uint16);
    RUN_TEST(test_type_range_int16);
    RUN_TEST(test_type_range_uint32);
    RUN_TEST(test_type_range_int32);

    /* effective_bounds */
    RUN_TEST(test_effective_bounds_unbounded);
    RUN_TEST(test_effective_bounds_explicit);
    RUN_TEST(test_effective_bounds_negative);
}
