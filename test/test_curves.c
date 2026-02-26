/*
 * test_curves.c — Curve definition tests (pure functions, no USB I/O)
 */

#include <stdio.h>
#include <string.h>
#include "helpers.h"
#include "alltrax.h"
#include "internal.h"
#include "test_internal.h"

/* ------------------------------------------------------------------ */
/* Lookup tests                                                        */
/* ------------------------------------------------------------------ */

static int test_find_linearization(void)
{
    const alltrax_curve_def* def = alltrax_find_curve("linearization");
    ASSERT_NOT_NULL(def);
    ASSERT_STR_EQ(def->name, "linearization");
    ASSERT_EQ(def->x_address, 0x08002100);
    ASSERT_EQ(def->y_address, 0x08002120);
    return 0;
}

static int test_find_speed(void)
{
    const alltrax_curve_def* def = alltrax_find_curve("speed");
    ASSERT_NOT_NULL(def);
    ASSERT_STR_EQ(def->name, "speed");
    ASSERT_EQ(def->x_address, 0x08002140);
    ASSERT_EQ(def->y_address, 0x08002160);
    return 0;
}

static int test_find_torque(void)
{
    const alltrax_curve_def* def = alltrax_find_curve("torque");
    ASSERT_NOT_NULL(def);
    ASSERT_STR_EQ(def->name, "torque");
    ASSERT_EQ(def->x_address, 0x08002180);
    ASSERT_EQ(def->y_address, 0x080021A0);
    return 0;
}

static int test_find_field(void)
{
    const alltrax_curve_def* def = alltrax_find_curve("field");
    ASSERT_NOT_NULL(def);
    ASSERT_STR_EQ(def->name, "field");
    ASSERT_EQ(def->x_address, 0x080021C0);
    ASSERT_EQ(def->y_address, 0x080021E0);
    return 0;
}

static int test_find_genfield(void)
{
    const alltrax_curve_def* def = alltrax_find_curve("genfield");
    ASSERT_NOT_NULL(def);
    ASSERT_STR_EQ(def->name, "genfield");
    ASSERT_EQ(def->x_address, 0x08002220);
    ASSERT_EQ(def->y_address, 0x08002240);
    return 0;
}

static int test_find_nonexistent(void)
{
    ASSERT_NULL(alltrax_find_curve("nosuchcurve"));
    ASSERT_NULL(alltrax_find_curve(""));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Count and index tests                                               */
/* ------------------------------------------------------------------ */

static int test_curve_count(void)
{
    ASSERT_EQ(alltrax_curve_count(), 5);
    return 0;
}

static int test_curve_by_index(void)
{
    const alltrax_curve_def* d0 = alltrax_curve_by_index(0);
    ASSERT_NOT_NULL(d0);
    ASSERT_STR_EQ(d0->name, "linearization");

    const alltrax_curve_def* d4 = alltrax_curve_by_index(4);
    ASSERT_NOT_NULL(d4);
    ASSERT_STR_EQ(d4->name, "genfield");

    ASSERT_NULL(alltrax_curve_by_index(5));
    ASSERT_NULL(alltrax_curve_by_index(100));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Address verification                                                */
/* ------------------------------------------------------------------ */

static int test_addresses_in_flash_pages(void)
{
    /* All user addresses must be in VARSET page (0x08002000-0x080027FF) */
    /* All factory addresses must be in FACTSET page (0x08001000-0x080017FF) */
    for (size_t i = 0; i < alltrax_curve_count(); i++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(i);

        /* User X */
        ASSERT_TRUE(def->x_address >= 0x08002000);
        ASSERT_TRUE(def->x_address + 32 <= 0x08002800);

        /* User Y */
        ASSERT_TRUE(def->y_address >= 0x08002000);
        ASSERT_TRUE(def->y_address + 32 <= 0x08002800);

        /* Factory X */
        ASSERT_TRUE(def->factory_x_address >= 0x08001000);
        ASSERT_TRUE(def->factory_x_address + 32 <= 0x08001800);

        /* Factory Y */
        ASSERT_TRUE(def->factory_y_address >= 0x08001000);
        ASSERT_TRUE(def->factory_y_address + 32 <= 0x08001800);

        /* X and Y arrays should not overlap */
        ASSERT_TRUE(def->x_address + 32 <= def->y_address ||
                     def->y_address + 32 <= def->x_address);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scale factor verification                                           */
/* ------------------------------------------------------------------ */

static int test_scale_factors(void)
{
    /* Throttle curves (lin, speed, torque): 20/819 for both X and Y */
    const double pct_scale = 20.0 / 819.0;

    for (size_t i = 0; i < 3; i++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(i);
        ASSERT_NEAR(def->x_scale, pct_scale, 1e-10);
        ASSERT_NEAR(def->y_scale, pct_scale, 1e-10);
        ASSERT_STR_EQ(def->x_unit, "%");
        ASSERT_STR_EQ(def->y_unit, "%");
        ASSERT_EQ(def->x_raw_max, 4095);
        ASSERT_EQ(def->y_raw_max, 4095);
    }

    /* Field curves: X=0.1, Y=0.01 */
    for (size_t i = 3; i < 5; i++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(i);
        ASSERT_NEAR(def->x_scale, 0.1, 1e-10);
        ASSERT_NEAR(def->y_scale, 0.01, 1e-10);
        ASSERT_STR_EQ(def->x_unit, "A");
        ASSERT_STR_EQ(def->y_unit, "A");
        ASSERT_EQ(def->x_raw_max, 7500);
        ASSERT_EQ(def->y_raw_max, 5000);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Factory address offset verification                                 */
/* ------------------------------------------------------------------ */

static int test_factory_offsets(void)
{
    /* Factory addresses should be at FACTSET page offset 0x100+
     * (same offset within their page as user addresses within VARSET) */
    for (size_t i = 0; i < alltrax_curve_count(); i++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(i);

        uint32_t user_x_offset = def->x_address - 0x08002000;
        uint32_t fact_x_offset = def->factory_x_address - 0x08001000;
        ASSERT_EQ(user_x_offset, fact_x_offset);

        uint32_t user_y_offset = def->y_address - 0x08002000;
        uint32_t fact_y_offset = def->factory_y_address - 0x08001000;
        ASSERT_EQ(user_y_offset, fact_y_offset);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Preset tests                                                        */
/* ------------------------------------------------------------------ */

static int test_preset_count(void)
{
    ASSERT_EQ(alltrax_curve_preset_count(), 7);
    return 0;
}

static int test_preset_by_index(void)
{
    const alltrax_curve_preset* p0 = alltrax_curve_preset_by_index(0);
    ASSERT_NOT_NULL(p0);
    ASSERT_STR_EQ(p0->curve_type, "linearization");

    const alltrax_curve_preset* p6 = alltrax_curve_preset_by_index(6);
    ASSERT_NOT_NULL(p6);
    ASSERT_STR_EQ(p6->curve_type, "torque");

    ASSERT_NULL(alltrax_curve_preset_by_index(7));
    ASSERT_NULL(alltrax_curve_preset_by_index(100));
    return 0;
}

static int test_preset_find(void)
{
    /* Linearization presets */
    const alltrax_curve_preset* p;

    p = alltrax_find_curve_preset("linearization", "linear");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "linear");

    p = alltrax_find_curve_preset("linearization", "0-5k-2wire");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->name, "0-5k-2wire");

    p = alltrax_find_curve_preset("linearization", "5k-0-2wire");
    ASSERT_NOT_NULL(p);

    p = alltrax_find_curve_preset("linearization", "yamaha");
    ASSERT_NOT_NULL(p);

    p = alltrax_find_curve_preset("linearization", "clubcar");
    ASSERT_NOT_NULL(p);

    /* Speed/torque presets */
    p = alltrax_find_curve_preset("speed", "standard");
    ASSERT_NOT_NULL(p);

    p = alltrax_find_curve_preset("torque", "standard");
    ASSERT_NOT_NULL(p);

    /* Non-existent */
    ASSERT_NULL(alltrax_find_curve_preset("linearization", "nosuch"));
    ASSERT_NULL(alltrax_find_curve_preset("field", "standard"));
    ASSERT_NULL(alltrax_find_curve_preset("speed", "linear"));

    return 0;
}

static int test_preset_data_s_curve(void)
{
    /* Verify 0-5k-2wire S-curve is correct */
    const alltrax_curve_preset* p =
        alltrax_find_curve_preset("linearization", "0-5k-2wire");
    ASSERT_NOT_NULL(p);

    /* Check a few key points from the S-curve */
    ASSERT_NEAR(p->x[1], 1.0, 0.01);
    ASSERT_NEAR(p->y[1], 0.0, 0.01);
    ASSERT_NEAR(p->x[5], 50.0, 0.01);
    ASSERT_NEAR(p->y[5], 15.0, 0.01);
    ASSERT_NEAR(p->x[11], 99.0, 0.01);
    ASSERT_NEAR(p->y[11], 100.0, 0.01);
    ASSERT_NEAR(p->x[12], 100.0, 0.01);
    ASSERT_NEAR(p->y[12], 100.0, 0.01);

    /* Trailing points should be zero */
    ASSERT_NEAR(p->x[13], 0.0, 0.01);
    ASSERT_NEAR(p->y[13], 0.0, 0.01);

    return 0;
}

static int test_preset_data_xct_torque(void)
{
    /* Verify XCT torque preset is correct */
    const alltrax_curve_preset* p =
        alltrax_find_curve_preset("torque", "standard");
    ASSERT_NOT_NULL(p);

    /* Aggressive curve: reaches 100% at 20% input */
    ASSERT_NEAR(p->x[2], 7.0, 0.01);
    ASSERT_NEAR(p->y[2], 69.0, 0.01);
    ASSERT_NEAR(p->x[6], 20.0, 0.01);
    ASSERT_NEAR(p->y[6], 100.0, 0.01);
    ASSERT_NEAR(p->x[7], 100.0, 0.01);
    ASSERT_NEAR(p->y[7], 100.0, 0.01);

    return 0;
}

/* ------------------------------------------------------------------ */
/* encode_curve_array                                                  */
/* ------------------------------------------------------------------ */

static int test_encode_curve_identity_scale(void)
{
    /* scale=1.0: raw = round(value / 1.0) */
    double values[ALLTRAX_CURVE_POINTS] = {0, 1, 100, -1, 0,0,0,0, 0,0,0,0, 0,0,0,0};
    uint8_t buf[ALLTRAX_CURVE_POINTS * 2];
    encode_curve_array(values, 1.0, buf);

    /* 0 → 0x0000 */
    ASSERT_EQ(buf[0], 0x00);
    ASSERT_EQ(buf[1], 0x00);
    /* 1 → 0x0001 */
    ASSERT_EQ(buf[2], 0x01);
    ASSERT_EQ(buf[3], 0x00);
    /* 100 → 0x0064 */
    ASSERT_EQ(buf[4], 0x64);
    ASSERT_EQ(buf[5], 0x00);
    /* -1 → 0xFFFF (int16 LE) */
    ASSERT_EQ(buf[6], 0xFF);
    ASSERT_EQ(buf[7], 0xFF);
    return 0;
}

static int test_encode_curve_pct_scale(void)
{
    /* scale = 20.0/819.0 ≈ 0.02442
     * 100% display → raw = round(100 / (20/819)) = round(4095) = 4095
     * 50% display → raw = round(50 / (20/819)) = round(2047.5) = 2048 */
    double scale = 20.0 / 819.0;
    double values[ALLTRAX_CURVE_POINTS] = {0};
    values[0] = 0.0;
    values[1] = 50.0;
    values[2] = 100.0;

    uint8_t buf[ALLTRAX_CURVE_POINTS * 2];
    encode_curve_array(values, scale, buf);

    /* 0 → raw 0 → 0x0000 */
    ASSERT_EQ(buf[0], 0x00);
    ASSERT_EQ(buf[1], 0x00);
    /* 50% → raw 2048 = 0x0800 */
    ASSERT_EQ(buf[2], 0x00);
    ASSERT_EQ(buf[3], 0x08);
    /* 100% → raw 4095 = 0x0FFF */
    ASSERT_EQ(buf[4], 0xFF);
    ASSERT_EQ(buf[5], 0x0F);
    return 0;
}

static int test_encode_curve_field_scale(void)
{
    /* Field X scale = 0.1: 750.0A → raw = round(750/0.1) = 7500 = 0x1D4C
     * Field Y scale = 0.01: 50.0A → raw = round(50/0.01) = 5000 = 0x1388 */
    double x_vals[ALLTRAX_CURVE_POINTS] = {0};
    double y_vals[ALLTRAX_CURVE_POINTS] = {0};
    x_vals[0] = 750.0;
    y_vals[0] = 50.0;

    uint8_t x_buf[ALLTRAX_CURVE_POINTS * 2];
    uint8_t y_buf[ALLTRAX_CURVE_POINTS * 2];
    encode_curve_array(x_vals, 0.1, x_buf);
    encode_curve_array(y_vals, 0.01, y_buf);

    /* 7500 = 0x1D4C LE */
    ASSERT_EQ(x_buf[0], 0x4C);
    ASSERT_EQ(x_buf[1], 0x1D);
    /* 5000 = 0x1388 LE */
    ASSERT_EQ(y_buf[0], 0x88);
    ASSERT_EQ(y_buf[1], 0x13);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

void run_curves_tests(void)
{
    printf("Curves:\n");
    RUN_TEST(test_find_linearization);
    RUN_TEST(test_find_speed);
    RUN_TEST(test_find_torque);
    RUN_TEST(test_find_field);
    RUN_TEST(test_find_genfield);
    RUN_TEST(test_find_nonexistent);
    RUN_TEST(test_curve_count);
    RUN_TEST(test_curve_by_index);
    RUN_TEST(test_addresses_in_flash_pages);
    RUN_TEST(test_scale_factors);
    RUN_TEST(test_factory_offsets);
    RUN_TEST(test_preset_count);
    RUN_TEST(test_preset_by_index);
    RUN_TEST(test_preset_find);
    RUN_TEST(test_preset_data_s_curve);
    RUN_TEST(test_preset_data_xct_torque);

    /* encode_curve_array */
    RUN_TEST(test_encode_curve_identity_scale);
    RUN_TEST(test_encode_curve_pct_scale);
    RUN_TEST(test_encode_curve_field_scale);
}
