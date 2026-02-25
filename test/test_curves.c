/*
 * test_curves.c — Curve definition tests (pure functions, no USB I/O)
 */

#include <stdio.h>
#include <string.h>
#include "helpers.h"
#include "alltrax.h"

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
}
