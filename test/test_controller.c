/*
 * Controller regression tests with a simulated transport.
 * This executable does not link transport.c or hidapi and cannot access USB.
 */

#include "helpers.h"
#include "internal.h"
#include <float.h>

int test_pass_count;
int test_fail_count;

static struct {
    alltrax_controller ctrl;
    uint8_t page[VARSET_SIZE];
    uint8_t factory[FLASH_PAGE_SIZE];
    uint8_t request[PACKET_SIZE];
    uint8_t mode;
    unsigned erases, settings_writes, goodset_clears;
    unsigned requests;
    uint32_t throttles_allowed;
    unsigned throttle_mask_reads, mode_writes;
    bool fault_enabled;
    uint32_t fault_addr;
    uint8_t fault_request_len, fault_reply_len;
    unsigned fault_occurrence, matches;
} mock;

static uint32_t request_address(void)
{
    const uint8_t* p = mock.request + 4;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void mock_reset(void)
{
    memset(&mock, 0, sizeof(mock));
    mock.ctrl.vid = ALLTRAX_VID;
    mock.ctrl.pid = ALLTRAX_PID_XCT;
    mock.ctrl.controller_type = ALLTRAX_CONTROLLER_XCT;
    mock.ctrl.allow_writes = true;
    mock.throttles_allowed = 0x7FF;
    for (size_t i = 0; i < sizeof(mock.page); i++)
        mock.page[i] = (uint8_t)(i * 37 + 11);
    mock.page[0] = mock.page[1] = 0;
    mock.page[0xA4] = 0;  /* HPD */
}

static void mock_fault(uint32_t addr, uint8_t request_len,
    uint8_t reply_len, unsigned occurrence)
{
    mock.fault_enabled = true;
    mock.fault_addr = addr;
    mock.fault_request_len = request_len;
    mock.fault_reply_len = reply_len;
    mock.fault_occurrence = occurrence;
}

static uint8_t mock_read_byte(uint32_t addr)
{
    static const char model[] = "XCT48400-DCS";
    static const uint8_t boot_rev[] = { 0x8A, 0x13, 0, 0 }; /* 5002 */
    static const uint8_t program_rev[] = { 0x8D, 0x13, 0, 0, 0, 0 }; /* 5005 */

    if (addr >= VARSET_ADDR && addr < VARSET_ADDR + VARSET_SIZE)
        return mock.page[addr - VARSET_ADDR];
    if (addr >= FACTSET_ADDR && addr < FACTSET_ADDR + FLASH_PAGE_SIZE)
        return mock.factory[addr - FACTSET_ADDR];
    if (addr >= ADDR_CONTROLLER_INFO && addr < ADDR_CONTROLLER_INFO + sizeof(model))
        return (uint8_t)model[addr - ADDR_CONTROLLER_INFO];
    if (addr >= ADDR_BOOT_REV && addr < ADDR_BOOT_REV + sizeof(boot_rev))
        return boot_rev[addr - ADDR_BOOT_REV];
    if (addr >= ADDR_PRGM_REV && addr < ADDR_PRGM_REV + sizeof(program_rev))
        return program_rev[addr - ADDR_PRGM_REV];
    if (addr == ADDR_RUN_MODE)
        return mock.mode;
    if (addr >= ADDR_HARDWARE_CONFIG + 16 && addr < ADDR_HARDWARE_CONFIG + 20)
        return (uint8_t)(mock.throttles_allowed >> ((addr - ADDR_HARDWARE_CONFIG - 16) * 8));
    return 0;
}

/* Only this test executable provides these internal transport symbols. */
alltrax_error transport_write(alltrax_controller* ctrl,
    const uint8_t data[PACKET_SIZE])
{
    mock.requests++;
    if (ctrl != &mock.ctrl || data[1] > MAX_PAYLOAD)
        return ALLTRAX_ERR_INVALID_ARG;
    memcpy(mock.request, data, PACKET_SIZE);
    uint32_t addr = request_address();

    if (data[0] == 0x01) {
        if (addr == ADDR_HARDWARE_CONFIG + 16)
            mock.throttle_mask_reads++;
        return ALLTRAX_OK;
    }
    if (data[0] == 0x02) {
        if (addr == ADDR_RUN_MODE && data[1] == 1) {
            mock.mode_writes++;
            mock.mode = data[8];
            return ALLTRAX_OK;
        }
        if (addr >= VARSET_ADDR && addr < VARSET_ADDR + VARSET_SIZE &&
            data[1] <= VARSET_ADDR + VARSET_SIZE - addr) {
            memcpy(mock.page + addr - VARSET_ADDR, data + 8, data[1]);
            mock.settings_writes++;
            if (addr == VARSET_ADDR && data[1] == 2 && data[8] == 0 && data[9] == 0)
                mock.goodset_clears++;
            return ALLTRAX_OK;
        }
    }
    if (data[0] == 0x03 && addr == 1 && data[1] == 4 && data[11] == VARSET_PAGE) {
        memset(mock.page, 0xFF, sizeof(mock.page));
        mock.erases++;
        return ALLTRAX_OK;
    }
    return ALLTRAX_ERR_INVALID_ARG;
}

alltrax_error transport_read(alltrax_controller* ctrl, uint8_t buf[PACKET_SIZE])
{
    if (ctrl != &mock.ctrl)
        return ALLTRAX_ERR_INVALID_ARG;
    memset(buf, 0, PACKET_SIZE);
    buf[0] = 0x04;
    if (mock.request[0] == 0x03) {
        /* Captured successful page-erase reply, independent of constants. */
        const uint8_t header[] = { 0x04, 0x02, 0, 0, 0x03, 0x01, 0, 0, 0xD4, 0x29 };
        memcpy(buf, header, sizeof(header));
        return ALLTRAX_OK;
    }
    buf[1] = mock.request[1];
    buf[4] = 0x01;
    if (mock.request[0] == 0x02) {
        memcpy(buf + 8, mock.request + 8, buf[1]);
        return ALLTRAX_OK;
    }

    uint32_t addr = request_address();
    if (mock.fault_enabled && addr == mock.fault_addr &&
        mock.request[1] == mock.fault_request_len &&
        ++mock.matches == mock.fault_occurrence)
        buf[1] = mock.fault_reply_len;

    /* Even an invalid declared length must not overflow the mock packet. */
    for (size_t i = 0; i < buf[1] && i < MAX_PAYLOAD; i++)
        buf[8 + i] = mock_read_byte(addr + (uint32_t)i);
    return ALLTRAX_OK;
}

static alltrax_error write_hpd(void)
{
    const alltrax_var_def* var = alltrax_find_var("HPD");
    double value = 1;
    return alltrax_write_vars(&mock.ctrl, &var, &value, 1, NULL);
}

static int test_string_assignments_rejected_before_device_access(void)
{
    const char* names[] = { "Throttle_Type_Name", "F_Table_Name" };
    const alltrax_write_opts unchecked = {
        .skip_cal = true, .skip_verify = true, .skip_goodset = true,
        .skip_fw_check = true, .skip_voltage_link = true,
    };
    const alltrax_write_opts* options[] = { NULL, &unchecked };

    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        for (size_t o = 0; o < sizeof(options) / sizeof(options[0]); o++) {
            /* Alone, and before/after valid RAM and FLASH assignments. */
            for (size_t pos = 0; pos < 4; pos++) {
                mock_reset();
                uint8_t original[VARSET_SIZE];
                memcpy(original, mock.page, sizeof(original));
                const alltrax_var_def* vars[3] = {
                    alltrax_find_var("Fan_On"), alltrax_find_var("HPD"), NULL,
                };
                double values[] = { 1, 1, 1 };
                size_t count = pos == 3 ? 1 : 3;
                size_t string_pos = pos == 3 ? 0 : pos;
                for (size_t i = 2; i > string_pos; i--)
                    vars[i] = vars[i - 1];
                vars[string_pos] = alltrax_find_var(names[n]);
                values[string_pos] = 123;

                ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars, values, count,
                                             options[o]), ALLTRAX_ERR_INVALID_ARG);
                ASSERT_EQ(mock.requests, 0);
                ASSERT_TRUE(strstr(mock.ctrl.error_detail, names[n]) != NULL);
                ASSERT_TRUE(strstr(mock.ctrl.error_detail, "string") != NULL);
                ASSERT_EQ(mock.erases, 0);
                ASSERT_EQ(mock.settings_writes, 0);
                ASSERT_EQ(mock.mode, RUN_MODE_RUN);
                ASSERT_MEM_EQ(mock.page, original, sizeof(original));
            }
        }
    }
    return 0;
}

static int test_unsupported_identity_blocks_all_operations(void)
{
    const struct {
        uint16_t vid, pid;
        alltrax_controller_type type;
    } identities[] = {
        { 0, 0, ALLTRAX_CONTROLLER_UNKNOWN },
        { 0x1234, ALLTRAX_PID_XCT, ALLTRAX_CONTROLLER_XCT },
        { ALLTRAX_VID, ALLTRAX_PID_SPM, ALLTRAX_CONTROLLER_XCT },
        { ALLTRAX_VID, 0x1234, ALLTRAX_CONTROLLER_XCT },
        { ALLTRAX_VID, ALLTRAX_PID_XCT, ALLTRAX_CONTROLLER_UNKNOWN },
        { ALLTRAX_VID, ALLTRAX_PID_XCT, ALLTRAX_CONTROLLER_SPM },
        { ALLTRAX_VID, ALLTRAX_PID_XCT, ALLTRAX_CONTROLLER_SR },
        { ALLTRAX_VID, ALLTRAX_PID_XCT, ALLTRAX_CONTROLLER_BMS },
        { ALLTRAX_VID, ALLTRAX_PID_XCT, ALLTRAX_CONTROLLER_BMS2 },
    };
    const alltrax_write_opts unchecked = {
        .skip_cal = true, .skip_verify = true, .skip_goodset = true,
        .skip_fw_check = true, .skip_voltage_link = true,
    };
    const alltrax_write_opts* options[] = { NULL, &unchecked };
    const alltrax_var_def* vars[] = {
        alltrax_find_var("Fan_On"), alltrax_find_var("HPD"),
    };
    const double values[] = { 1, 1 };
    const alltrax_curve_def* curve = alltrax_find_curve("linearization");
    alltrax_curve_data data = {0};

    for (size_t i = 0; i < sizeof(identities) / sizeof(identities[0]); i++) {
        mock_reset();
        mock.ctrl.vid = identities[i].vid;
        mock.ctrl.pid = identities[i].pid;
        mock.ctrl.controller_type = identities[i].type;
        uint8_t original[VARSET_SIZE];
        memcpy(original, mock.page, sizeof(original));

        for (size_t o = 0; o < sizeof(options) / sizeof(options[0]); o++) {
            for (size_t v = 0; v < 2; v++) {
                ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars + v, values + v,
                                             1, options[o]), ALLTRAX_ERR_UNSUPPORTED);
            }
            ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars, values, 2, options[o]),
                      ALLTRAX_ERR_UNSUPPORTED);
            ASSERT_EQ(alltrax_write_curve(&mock.ctrl, curve, &data, options[o]),
                      ALLTRAX_ERR_UNSUPPORTED);
        }
        ASSERT_EQ(alltrax_reset_device(&mock.ctrl), ALLTRAX_ERR_UNSUPPORTED);
        alltrax_info info;
        alltrax_var_value value;
        alltrax_monitor_data monitor;
        ASSERT_EQ(alltrax_get_info(&mock.ctrl, &info), ALLTRAX_ERR_UNSUPPORTED);
        ASSERT_EQ(alltrax_read_vars(&mock.ctrl, vars, 1, &value), ALLTRAX_ERR_UNSUPPORTED);
        ASSERT_EQ(alltrax_read_curve(&mock.ctrl, curve, &data), ALLTRAX_ERR_UNSUPPORTED);
        ASSERT_EQ(alltrax_read_curve_factory(&mock.ctrl, curve, &data), ALLTRAX_ERR_UNSUPPORTED);
        ASSERT_EQ(alltrax_read_monitor(&mock.ctrl, &monitor), ALLTRAX_ERR_UNSUPPORTED);
        ASSERT_TRUE(strstr(mock.ctrl.error_detail, "requires") != NULL);
        ASSERT_EQ(mock.requests, 0);
        ASSERT_MEM_EQ(mock.page, original, sizeof(original));
        ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    }
    return 0;
}

static int test_read_only_variables_reject_entire_batch_before_io(void)
{
    const char* names[] = {
        "Global_Shutdown", "Var_GoodSet", "F_Reverse_Field_Weaken_Percent",
        "SerialNum", "Model",
    };
    const alltrax_write_opts unchecked = {
        .skip_cal = true, .skip_verify = true, .skip_goodset = true,
        .skip_fw_check = true, .skip_voltage_link = true,
    };
    const alltrax_write_opts* options[] = { NULL, &unchecked };

    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        const alltrax_var_def* read_only = alltrax_find_var(names[n]);
        ASSERT_TRUE(read_only != NULL);
        ASSERT_FALSE(read_only->writable);
        for (size_t o = 0; o < sizeof(options) / sizeof(options[0]); o++) {
            /* Alone, or at every position among valid RAM/FLASH assignments. */
            for (size_t pos = 0; pos < 4; pos++) {
                mock_reset();
                uint8_t original[VARSET_SIZE];
                memcpy(original, mock.page, sizeof(original));
                const alltrax_var_def* vars[3] = {
                    alltrax_find_var("Fan_On"), alltrax_find_var("HPD"), NULL,
                };
                const double values[] = { 1, 1, 1 };
                size_t count = pos == 3 ? 1 : 3;
                size_t read_only_pos = pos == 3 ? 0 : pos;
                for (size_t i = 2; i > read_only_pos; i--)
                    vars[i] = vars[i - 1];
                vars[read_only_pos] = read_only;

                ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars, values, count,
                                             options[o]), ALLTRAX_ERR_INVALID_ARG);
                ASSERT_EQ(mock.requests, 0);
                ASSERT_TRUE(strstr(mock.ctrl.error_detail, names[n]) != NULL);
                ASSERT_TRUE(strstr(mock.ctrl.error_detail, "read-only") != NULL);
                ASSERT_EQ(mock.erases, 0);
                ASSERT_EQ(mock.settings_writes, 0);
                ASSERT_EQ(mock.goodset_clears, 0);
                ASSERT_MEM_EQ(mock.page, original, sizeof(original));
                ASSERT_EQ(mock.mode, RUN_MODE_RUN);
            }
        }
    }
    return 0;
}

static int test_invalid_numeric_batches_reject_before_io(void)
{
    const double invalid[] = {
        NAN, INFINITY, -INFINITY, DBL_MAX, -DBL_MAX, 1e100, -1e100,
        0x1p63, -0x1p63, 999.49, 8000.5,
    };
    const alltrax_write_opts unchecked = {
        .skip_cal = true, .skip_verify = true, .skip_goodset = true,
        .skip_fw_check = true, .skip_voltage_link = true,
    };
    const alltrax_write_opts* options[] = { NULL, &unchecked };
    for (size_t v = 0; v < sizeof(invalid) / sizeof(invalid[0]); v++) {
        for (size_t o = 0; o < sizeof(options) / sizeof(options[0]); o++) {
            for (size_t pos = 0; pos < 4; pos++) {
                mock_reset();
                uint8_t original[VARSET_SIZE];
                memcpy(original, mock.page, sizeof(original));
                const alltrax_var_def* vars[3] = {
                    alltrax_find_var("Fan_On"), alltrax_find_var("HPD"), NULL,
                };
                double values[] = { 1, 1, 1 };
                size_t count = pos == 3 ? 1 : 3;
                size_t bad_pos = pos == 3 ? 0 : pos;
                for (size_t i = 2; i > bad_pos; i--)
                    vars[i] = vars[i - 1];
                vars[bad_pos] = alltrax_find_var("N_Speed_Limit");
                values[bad_pos] = invalid[v];
                ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars, values, count,
                                             options[o]), ALLTRAX_ERR_INVALID_ARG);
                ASSERT_EQ(mock.requests, 0);
                ASSERT_TRUE(strstr(mock.ctrl.error_detail, "N_Speed_Limit") != NULL);
                ASSERT_MEM_EQ(mock.page, original, sizeof(original));
                ASSERT_EQ(mock.mode, RUN_MODE_RUN);
            }
        }
    }
    /* Boolean RAM writes must reject nonfinite values as well. */
    for (size_t v = 0; v < 3; v++) {
        mock_reset();
        const alltrax_var_def* fan = alltrax_find_var("Fan_On");
        ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &fan, invalid + v, 1, NULL),
                  ALLTRAX_ERR_INVALID_ARG);
        ASSERT_EQ(mock.requests, 0);
    }
    return 0;
}

static int test_scaled_flash_batch_preserves_values_and_neighbors(void)
{
    mock_reset();
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, mock.page, sizeof(expected));
    expected[0x20] = 0x19; expected[0x21] = 0x01; /* 28.1V -> 281 */
    expected[0x44] = 0x00; expected[0x45] = 0xF8; /* -50% -> -2048 */
    const alltrax_var_def* vars[] = {
        alltrax_find_var("LoBat_Vlim"), alltrax_find_var("N_Max_Batt_Regen_Amps"),
    };
    const double values[] = { 28.1, -50 };
    const alltrax_write_opts opts = { .skip_voltage_link = true };
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars, values, 2, &opts), ALLTRAX_OK);
    ASSERT_MEM_EQ(mock.page, expected, sizeof(expected));
    ASSERT_EQ(mock.erases, 1);
    ASSERT_EQ(mock.goodset_clears, 1);
    ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    return 0;
}

static int test_invalid_curve_points_reject_before_io(void)
{
    const double invalid[] = { NAN, INFINITY, -INFINITY, DBL_MAX, -DBL_MAX, -1, 200 };
    const alltrax_write_opts unchecked = {
        .skip_cal = true, .skip_verify = true, .skip_goodset = true,
        .skip_fw_check = true, .skip_voltage_link = true,
    };
    const alltrax_write_opts* options[] = { NULL, &unchecked };
    for (size_t c = 0; c < alltrax_curve_count(); c++) {
        const alltrax_curve_def* def = alltrax_curve_by_index(c);
        for (int axis = 0; axis < 2; axis++) {
            double scale = axis ? def->y_scale : def->x_scale;
            int max = axis ? def->y_raw_max : def->x_raw_max;
            for (size_t v = 0; v < sizeof(invalid) / sizeof(invalid[0]); v++) {
                for (size_t o = 0; o < sizeof(options) / sizeof(options[0]); o++) {
                    for (int p = 0; p < ALLTRAX_CURVE_POINTS; p++) {
                        mock_reset();
                        uint8_t original[VARSET_SIZE];
                        memcpy(original, mock.page, sizeof(original));
                        alltrax_curve_data data = { .def = def };
                        /* The last case is beyond each axis's actual maximum. */
                        double value = v == 6 ? (max + 1) * scale : invalid[v];
                        (axis ? data.y : data.x)[p] = value;
                        ASSERT_EQ(alltrax_write_curve(&mock.ctrl, def, &data, options[o]),
                                  ALLTRAX_ERR_INVALID_ARG);
                        ASSERT_EQ(mock.requests, 0);
                        ASSERT_TRUE(strstr(mock.ctrl.error_detail, def->name) != NULL);
                        ASSERT_MEM_EQ(mock.page, original, sizeof(original));
                        ASSERT_EQ(mock.mode, RUN_MODE_RUN);
                    }
                }
            }
        }
    }
    return 0;
}

static int test_curve_write_preserves_padding_and_neighbors(void)
{
    mock_reset();
    const alltrax_curve_def* def = alltrax_find_curve("linearization");
    const alltrax_curve_preset* preset = alltrax_find_curve_preset("linearization", "linear");
    alltrax_curve_data data = { .def = def };
    memcpy(data.x, preset->x, sizeof(data.x));
    memcpy(data.y, preset->y, sizeof(data.y));
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, mock.page, sizeof(expected));
    /* Literal C encoding of the padded linear preset, using existing round(). */
    const uint8_t x[32] = { 0,0, 41,0, 0xD6,0x0F, 0xFF,0x0F };
    const uint8_t y[32] = { 0,0, 0,0, 0xFF,0x0F, 0xFF,0x0F };
    memcpy(expected + 0x100, x, sizeof(x));
    memcpy(expected + 0x120, y, sizeof(y));
    ASSERT_EQ(alltrax_write_curve(&mock.ctrl, def, &data, NULL), ALLTRAX_OK);
    ASSERT_MEM_EQ(mock.page, expected, sizeof(expected));
    ASSERT_EQ(mock.erases, 1);
    ASSERT_EQ(mock.goodset_clears, 1);
    ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    return 0;
}

static int test_factory_curves_round_trip_within_declared_bounds(void)
{
    for (size_t c = 0; c < alltrax_curve_count(); c++) {
        mock_reset();
        const alltrax_curve_def* def = alltrax_curve_by_index(c);
        uint8_t raw[2][32] = {{0}};
        raw[0][2] = (uint8_t)def->x_raw_max;
        raw[0][3] = (uint8_t)(def->x_raw_max >> 8);
        raw[1][2] = (uint8_t)def->y_raw_max;
        raw[1][3] = (uint8_t)(def->y_raw_max >> 8);
        memcpy(mock.factory + def->factory_x_address - FACTSET_ADDR, raw[0], 32);
        memcpy(mock.factory + def->factory_y_address - FACTSET_ADDR, raw[1], 32);
        uint8_t expected[VARSET_SIZE];
        memcpy(expected, mock.page, sizeof(expected));
        memcpy(expected + def->x_address - VARSET_ADDR, raw[0], 32);
        memcpy(expected + def->y_address - VARSET_ADDR, raw[1], 32);
        alltrax_curve_data data;
        ASSERT_EQ(alltrax_read_curve_factory(&mock.ctrl, def, &data), ALLTRAX_OK);
        ASSERT_EQ(alltrax_validate_curve(def, &data), ALLTRAX_OK);
        ASSERT_EQ(alltrax_write_curve(&mock.ctrl, def, &data, NULL), ALLTRAX_OK);
        ASSERT_MEM_EQ(mock.page, expected, sizeof(expected));
        ASSERT_EQ(mock.erases, 1);
        ASSERT_EQ(mock.goodset_clears, 1);
        ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    }
    return 0;
}

static int test_invalid_throttle_types_reject_batch_before_io(void)
{
    const double invalid[] = { 33, 12, 32, 255, -1, -0.1, 0.49, 0.5, 1.9, 10.9, 11.1 };
    const alltrax_write_opts unchecked = {
        .skip_cal = true, .skip_verify = true, .skip_goodset = true,
        .skip_fw_check = true, .skip_voltage_link = true,
    };
    const alltrax_write_opts* options[] = { NULL, &unchecked };
    for (size_t v = 0; v < sizeof(invalid) / sizeof(invalid[0]); v++) {
        for (size_t o = 0; o < sizeof(options) / sizeof(options[0]); o++) {
            for (size_t pos = 0; pos < 4; pos++) {
                mock_reset();
                uint8_t original[VARSET_SIZE];
                memcpy(original, mock.page, sizeof(original));
                const alltrax_var_def* vars[3] = {
                    alltrax_find_var("Fan_On"), alltrax_find_var("HPD"), NULL,
                };
                double values[] = { 1, 1, 1 };
                size_t count = pos == 3 ? 1 : 3;
                size_t bad_pos = pos == 3 ? 0 : pos;
                for (size_t i = 2; i > bad_pos; i--) vars[i] = vars[i - 1];
                vars[bad_pos] = alltrax_find_var("Throttle_Type");
                values[bad_pos] = invalid[v];
                ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars, values, count, options[o]),
                          ALLTRAX_ERR_INVALID_ARG);
                ASSERT_EQ(mock.requests, 0);
                ASSERT_MEM_EQ(mock.page, original, sizeof(original));
                ASSERT_EQ(mock.mode_writes, 0);
                ASSERT_EQ(mock.erases, 0);
                ASSERT_TRUE(strstr(mock.ctrl.error_detail, "Throttle_Type") != NULL);
            }
        }
    }
    return 0;
}

static int test_nonzero_throttle_types_require_their_exact_hardware_bit(void)
{
    const uint32_t bits[] = { 0x001,0x002,0x004,0x008,0x010,0x020,0x040,0x080,0x100,0x200,0x400 };
    const alltrax_var_def* var = alltrax_find_var("Throttle_Type");
    const alltrax_write_opts unchecked = {
        .skip_cal = true, .skip_verify = true, .skip_goodset = true,
        .skip_fw_check = true, .skip_voltage_link = true,
    };
    const alltrax_write_opts* options[] = { NULL, &unchecked };
    for (size_t t = 0; t < sizeof(bits) / sizeof(bits[0]); t++) {
        for (int allowed = 0; allowed < 2; allowed++) {
            for (size_t o = 0; o < sizeof(options) / sizeof(options[0]); o++) {
                mock_reset();
                mock.throttles_allowed = allowed ? bits[t] : ~bits[t];
                uint8_t expected[VARSET_SIZE];
                memcpy(expected, mock.page, sizeof(expected));
                double value = (double)t + 1;
                ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, &value, 1, options[o]),
                          allowed ? ALLTRAX_OK : ALLTRAX_ERR_INVALID_ARG);
                ASSERT_EQ(mock.throttle_mask_reads, 1);
                if (allowed) expected[0xA0] = (uint8_t)(t + 1);
                ASSERT_MEM_EQ(mock.page, expected, sizeof(expected));
                ASSERT_EQ(mock.erases, allowed);
                ASSERT_EQ(mock.goodset_clears, allowed);
                ASSERT_EQ(mock.mode_writes, allowed && o == 0 ? 2 : 0);
                ASSERT_EQ(mock.mode, RUN_MODE_RUN);
            }
        }
    }
    return 0;
}

static int test_throttle_none_bypasses_hardware_mask(void)
{
    mock_reset();
    mock.throttles_allowed = 0;
    mock_fault(ADDR_HARDWARE_CONFIG + 16, 4, 0, 1);
    const alltrax_var_def* var = alltrax_find_var("Throttle_Type");
    double value = 0;
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, &value, 1, NULL), ALLTRAX_OK);
    ASSERT_EQ(mock.throttle_mask_reads, 0);
    ASSERT_EQ(mock.page[0xA0], 0);
    ASSERT_EQ(mock.erases, 1);
    ASSERT_EQ(mock.mode_writes, 2);
    ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    return 0;
}

static int test_every_throttle_assignment_is_checked(void)
{
    const alltrax_var_def* vars[] = {
        alltrax_find_var("Throttle_Type"), alltrax_find_var("Throttle_Type"),
    };
    const double values[][2] = { {0,1}, {1,2} };
    for (size_t i = 0; i < 2; i++) {
        mock_reset();
        mock.throttles_allowed = i == 0 ? 0x002 : 0x001;
        uint8_t original[VARSET_SIZE];
        memcpy(original, mock.page, sizeof(original));
        ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars, values[i], 2, NULL), ALLTRAX_ERR_INVALID_ARG);
        ASSERT_EQ(mock.throttle_mask_reads, i + 1);
        ASSERT_EQ(mock.mode_writes, 0);
        ASSERT_EQ(mock.erases, 0);
        ASSERT_MEM_EQ(mock.page, original, sizeof(original));
    }
    return 0;
}

static int test_throttle_mask_uses_encoded_value(void)
{
    /* Address-bearing descriptors are still public. Even a modified descriptor
     * must check the byte it will encode, and reject undefined encoded types. */
    alltrax_var_def shifted = *alltrax_find_var("Throttle_Type");
    shifted.offset = 1;
    const alltrax_var_def* var = &shifted;
    double value = 1; /* This descriptor encodes type 2. */
    mock_reset();
    mock.throttles_allowed = 0x001;
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, &value, 1, NULL), ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(mock.mode_writes, 0);
    ASSERT_EQ(mock.erases, 0);
    mock_reset();
    mock.throttles_allowed = 0x002;
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, &value, 1, NULL), ALLTRAX_OK);
    ASSERT_EQ(mock.page[0xA0], 2);
    mock_reset();
    shifted.offset = 32;
    shifted.raw_max = 255; /* Encoded 33 must never reach the shift. */
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, &value, 1, NULL), ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(mock.requests, 0);
    return 0;
}

static int test_info_exact_read_lengths(void)
{
    mock_reset();
    alltrax_info info;
    ASSERT_EQ(alltrax_get_info(&mock.ctrl, &info), ALLTRAX_OK);
    ASSERT_STR_EQ(info.model, "XCT48400-DCS");
    ASSERT_EQ(info.boot_rev, 5002);
    ASSERT_EQ(info.program_rev, 5005);
    return 0;
}

static int test_info_rejects_short_payloads(void)
{
    const uint8_t lengths[] = { 0, 3 };
    for (size_t i = 0; i < sizeof(lengths); i++) {
        mock_reset();
        mock_fault(ADDR_BOOT_REV, 4, lengths[i], 1);
        alltrax_info info;
        ASSERT_EQ(alltrax_get_info(&mock.ctrl, &info), ALLTRAX_ERR_PROTOCOL);
    }
    return 0;
}

static int test_info_rejects_oversized_payloads(void)
{
    const uint8_t lengths[] = { 5, 56, 57 };
    for (size_t i = 0; i < sizeof(lengths); i++) {
        mock_reset();
        mock_fault(ADDR_BOOT_REV, 4, lengths[i], 1);
        alltrax_info info;
        ASSERT_EQ(alltrax_get_info(&mock.ctrl, &info), ALLTRAX_ERR_PROTOCOL);
    }
    return 0;
}

static int test_read_vars_rejects_length_mismatch(void)
{
    const uint8_t lengths[] = { 0, 3, 5, 56 };
    const alltrax_var_def* vars[] = {
        alltrax_find_var("LoBat_Vlim"), alltrax_find_var("HiBat_Vlim"),
    };
    for (size_t i = 0; i < sizeof(lengths); i++) {
        mock_reset();
        mock_fault(vars[0]->address, 4, lengths[i], 1);
        alltrax_var_value out[2] = {0};
        ASSERT_EQ(alltrax_read_vars(&mock.ctrl, vars, 2, out), ALLTRAX_ERR_PROTOCOL);
        ASSERT_NULL(out[0].def);
        ASSERT_NULL(out[1].def);
    }
    return 0;
}

static int test_flash_write_preserves_page(void)
{
    mock_reset();
    uint8_t expected[VARSET_SIZE];
    memcpy(expected, mock.page, sizeof(expected));
    expected[0xA4] = 1;
    ASSERT_EQ(write_hpd(), ALLTRAX_OK);
    ASSERT_MEM_EQ(mock.page, expected, sizeof(expected));
    ASSERT_EQ(mock.erases, 1);
    ASSERT_EQ(mock.goodset_clears, 1);
    ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    return 0;
}

static int test_flash_write_aborts_on_incomplete_page_read(void)
{
    const uint8_t lengths[] = { 0, 55 };
    for (size_t i = 0; i < sizeof(lengths); i++) {
        mock_reset();
        uint8_t original[VARSET_SIZE];
        memcpy(original, mock.page, sizeof(original));
        mock_fault(VARSET_ADDR + MAX_PAYLOAD, MAX_PAYLOAD, lengths[i], 1);
        ASSERT_EQ(write_hpd(), ALLTRAX_ERR_PROTOCOL);
        ASSERT_EQ(mock.erases, 0);
        ASSERT_EQ(mock.settings_writes, 0);
        ASSERT_MEM_EQ(mock.page, original, sizeof(original));
        ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    }
    return 0;
}

static int test_flash_write_aborts_on_oversized_final_chunk(void)
{
    const uint8_t lengths[] = { 33, 56 };
    for (size_t i = 0; i < sizeof(lengths); i++) {
        mock_reset();
        uint8_t original[VARSET_SIZE];
        memcpy(original, mock.page, sizeof(original));
        mock_fault(VARSET_ADDR + VARSET_SIZE - 32, 32, lengths[i], 1);
        ASSERT_EQ(write_hpd(), ALLTRAX_ERR_PROTOCOL);
        ASSERT_EQ(mock.erases, 0);
        ASSERT_EQ(mock.settings_writes, 0);
        ASSERT_MEM_EQ(mock.page, original, sizeof(original));
        ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    }
    return 0;
}

static int test_flash_write_rejects_incomplete_verification(void)
{
    const uint8_t lengths[] = { 0, 55 };
    for (size_t i = 0; i < sizeof(lengths); i++) {
        mock_reset();
        mock_fault(VARSET_ADDR + MAX_PAYLOAD, MAX_PAYLOAD, lengths[i], 2);
        ASSERT_EQ(write_hpd(), ALLTRAX_ERR_PROTOCOL);
        ASSERT_EQ(mock.erases, 1);
        ASSERT_EQ(mock.goodset_clears, 0);
        ASSERT_EQ(get_le16(mock.page), 0xFFFF);
        ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    }
    return 0;
}

static int test_flash_write_rejects_final_verification_length_mismatch(void)
{
    const uint8_t lengths[] = { 0, 31, 33, 56 };
    for (size_t i = 0; i < sizeof(lengths); i++) {
        mock_reset();
        mock_fault(VARSET_ADDR + VARSET_SIZE - 32, 32, lengths[i], 2);
        ASSERT_EQ(write_hpd(), ALLTRAX_ERR_PROTOCOL);
        ASSERT_EQ(mock.erases, 1);
        ASSERT_EQ(mock.goodset_clears, 0);
        ASSERT_EQ(get_le16(mock.page), 0xFFFF);
        ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    }
    return 0;
}

int main(void)
{
    printf("controller (simulated transport):\n");
    RUN_TEST(test_info_exact_read_lengths);
    RUN_TEST(test_info_rejects_short_payloads);
    RUN_TEST(test_info_rejects_oversized_payloads);
    RUN_TEST(test_read_vars_rejects_length_mismatch);
    RUN_TEST(test_flash_write_preserves_page);
    RUN_TEST(test_flash_write_aborts_on_incomplete_page_read);
    RUN_TEST(test_flash_write_aborts_on_oversized_final_chunk);
    RUN_TEST(test_flash_write_rejects_incomplete_verification);
    RUN_TEST(test_flash_write_rejects_final_verification_length_mismatch);
    RUN_TEST(test_string_assignments_rejected_before_device_access);
    RUN_TEST(test_unsupported_identity_blocks_all_operations);
    RUN_TEST(test_read_only_variables_reject_entire_batch_before_io);
    RUN_TEST(test_invalid_numeric_batches_reject_before_io);
    RUN_TEST(test_scaled_flash_batch_preserves_values_and_neighbors);
    RUN_TEST(test_invalid_curve_points_reject_before_io);
    RUN_TEST(test_curve_write_preserves_padding_and_neighbors);
    RUN_TEST(test_factory_curves_round_trip_within_declared_bounds);
    RUN_TEST(test_invalid_throttle_types_reject_batch_before_io);
    RUN_TEST(test_nonzero_throttle_types_require_their_exact_hardware_bit);
    RUN_TEST(test_throttle_none_bypasses_hardware_mask);
    RUN_TEST(test_every_throttle_assignment_is_checked);
    RUN_TEST(test_throttle_mask_uses_encoded_value);
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
