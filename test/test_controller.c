/*
 * Controller regression tests with a simulated transport.
 * This executable does not link transport.c or hidapi and cannot access USB.
 */

#include "helpers.h"
#include "internal.h"
#include "test_internal.h"
#include <math.h>
#include <float.h>

int test_pass_count;
int test_fail_count;

typedef enum {
    MODE_REPLY_OK, MODE_REPLY_TIMEOUT, MODE_REPLY_USB, MODE_REPLY_BAD_HEADER,
    MODE_REPLY_BAD_LENGTH, MODE_REPLY_BAD_ECHO, MODE_REPLY_DEVICE_FAIL,
} mode_reply_fault;

static struct {
    alltrax_controller ctrl;
    uint8_t page[VARSET_SIZE];
    uint8_t factory[FLASH_PAGE_SIZE];
    uint8_t ram_tail[4];
    uint8_t request[PACKET_SIZE];
    uint8_t mode;
    unsigned erases, settings_writes, goodset_clears;
    unsigned requests;
    uint32_t throttles_allowed;
    uint16_t rated_volts;
    uint32_t orig_boot_rev, orig_program_rev;
    unsigned hw_config_reads, mode_writes;
    unsigned cal_attempts, run_attempts, fan_writes;
    bool cal_transfer_error, run_transfer_error, fan_transfer_error;
    bool cal_write_blocked;
    mode_reply_fault cal_reply, run_reply;
    bool fault_enabled;
    uint32_t fault_addr;
    uint8_t fault_request_len, fault_reply_len;
    unsigned fault_occurrence, matches;
    uint32_t boot_rev, program_rev;
    uint8_t monitor_ram[0x200];  /* 0x2000F000-0x2000F1FF */
    size_t stuck_offset;         /* 1-based VARSET offset that stays 0xFF */
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
    mock.ctrl.boot_rev = mock.boot_rev = 5002;
    mock.ctrl.program_rev = mock.program_rev = 5005;
    mock.throttles_allowed = 0x7FF;
    mock.rated_volts = 48;
    mock.orig_boot_rev = 5002;
    mock.orig_program_rev = 5005;
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

    if (addr >= VARSET_ADDR && addr < VARSET_ADDR + VARSET_SIZE)
        return mock.page[addr - VARSET_ADDR];
    if (addr >= FACTSET_ADDR && addr < FACTSET_ADDR + FLASH_PAGE_SIZE)
        return mock.factory[addr - FACTSET_ADDR];
    if (addr >= ADDR_CONTROLLER_INFO && addr < ADDR_CONTROLLER_INFO + sizeof(model))
        return (uint8_t)model[addr - ADDR_CONTROLLER_INFO];
    if (addr >= ADDR_BOOT_REV && addr < ADDR_BOOT_REV + 4)
        return (uint8_t)(mock.boot_rev >> ((addr - ADDR_BOOT_REV) * 8));
    if (addr >= ADDR_PRGM_REV && addr < ADDR_PRGM_REV + 4)
        return (uint8_t)(mock.program_rev >> ((addr - ADDR_PRGM_REV) * 8));
    if (addr >= 0x2000F000 && addr < 0x2000F000 + sizeof(mock.monitor_ram))
        return mock.monitor_ram[addr - 0x2000F000];
    if (addr == ADDR_RUN_MODE)
        return mock.mode;
    if (addr >= RAM_END - 4 && addr < RAM_END)
        return mock.ram_tail[addr - (RAM_END - 4)];
    if (addr >= ADDR_CONTROLLER_INFO + 0x24 && addr < ADDR_CONTROLLER_INFO + 0x28)
        return (uint8_t)(mock.orig_boot_rev >> ((addr - ADDR_CONTROLLER_INFO - 0x24) * 8));
    if (addr >= ADDR_CONTROLLER_INFO + 0x28 && addr < ADDR_CONTROLLER_INFO + 0x2C)
        return (uint8_t)(mock.orig_program_rev >> ((addr - ADDR_CONTROLLER_INFO - 0x28) * 8));
    if (addr >= ADDR_HARDWARE_CONFIG && addr < ADDR_HARDWARE_CONFIG + 2)
        return (uint8_t)(mock.rated_volts >> ((addr - ADDR_HARDWARE_CONFIG) * 8));
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
        if (addr == ADDR_HARDWARE_CONFIG)
            mock.hw_config_reads++;
        return ALLTRAX_OK;
    }
    if (data[0] == 0x02) {
        if (addr == ADDR_RUN_MODE && data[1] == 1) {
            if (data[8] == RUN_MODE_CAL && mock.cal_write_blocked) {
                set_error_detail(ctrl, "CAL write blocked before send");
                return ALLTRAX_ERR_BLOCKED;
            }
            mock.mode_writes++;
            if (data[8] == RUN_MODE_CAL) {
                mock.cal_attempts++;
                /* CAL may have taken effect even when send reports failure. */
                mock.mode = data[8];
                if (mock.cal_transfer_error) {
                    set_error_detail(ctrl, "CAL transfer failure");
                    return ALLTRAX_ERR_USB;
                }
            } else if (data[8] == RUN_MODE_RUN) {
                mock.run_attempts++;
                if (mock.run_transfer_error) {
                    set_error_detail(ctrl, "RUN transfer failure");
                    return ALLTRAX_ERR_USB;
                }
            }
            mock.mode = data[8];
            return ALLTRAX_OK;
        }
        if (addr == 0x2000F204 && data[1] == 1) {
            if (mock.fan_transfer_error) {
                set_error_detail(ctrl, "Fan_On transfer failure");
                return ALLTRAX_ERR_USB;
            }
            mock.fan_writes++;
            return ALLTRAX_OK;
        }
        if (addr >= RAM_END - 4 && addr < RAM_END && data[1] <= RAM_END - addr) {
            memcpy(mock.ram_tail + (addr - (RAM_END - 4)), data + 8, data[1]);
            return ALLTRAX_OK;
        }
        if (addr >= VARSET_ADDR && addr < VARSET_ADDR + VARSET_SIZE &&
            data[1] <= VARSET_ADDR + VARSET_SIZE - addr) {
            memcpy(mock.page + (addr - VARSET_ADDR), data + 8, data[1]);
            if (mock.stuck_offset &&
                mock.stuck_offset - 1 - (addr - VARSET_ADDR) < data[1])
                mock.page[mock.stuck_offset - 1] = 0xFF;  /* Erased value */
            mock.settings_writes++;
            if (addr == VARSET_ADDR && data[1] == 2 && data[8] == 0 && data[9] == 0)
                mock.goodset_clears++;
            return ALLTRAX_OK;
        }
    }
    if (data[0] == 0x03 && addr == FUNC_RESET && data[1] == 0)
        return ALLTRAX_OK;
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
        if (request_address() == ADDR_RUN_MODE) {
            bool cal = mock.request[8] == RUN_MODE_CAL;
            mode_reply_fault fault = cal ? mock.cal_reply : mock.run_reply;
            switch (fault) {
            case MODE_REPLY_OK: break;
            case MODE_REPLY_TIMEOUT:
                set_error_detail(ctrl, "%s acknowledgement timeout", cal ? "CAL" : "RUN");
                return ALLTRAX_ERR_TIMEOUT;
            case MODE_REPLY_USB:
                set_error_detail(ctrl, "%s acknowledgement USB failure", cal ? "CAL" : "RUN");
                return ALLTRAX_ERR_USB;
            case MODE_REPLY_BAD_HEADER: buf[0] = 0x05; break;
            case MODE_REPLY_BAD_LENGTH: buf[1] = 0; break;
            case MODE_REPLY_BAD_ECHO: buf[8] ^= 1; break;
            case MODE_REPLY_DEVICE_FAIL: buf[6] = 1; break;
            }
        }
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

static const alltrax_write_opts batch_opts = { .skip_voltage_link = true };

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
        ASSERT_EQ(alltrax_reset_device(&mock.ctrl, NULL), ALLTRAX_ERR_UNSUPPORTED);
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
    /* The padded linear preset. */
    const uint8_t x[32] = { 0,0, 40,0, 0xD6,0x0F, 0xFF,0x0F };
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
        memcpy(mock.factory + (def->factory_x_address - FACTSET_ADDR), raw[0], 32);
        memcpy(mock.factory + (def->factory_y_address - FACTSET_ADDR), raw[1], 32);
        uint8_t expected[VARSET_SIZE];
        memcpy(expected, mock.page, sizeof(expected));
        memcpy(expected + (def->x_address - VARSET_ADDR), raw[0], 32);
        memcpy(expected + (def->y_address - VARSET_ADDR), raw[1], 32);
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
                ASSERT_EQ(mock.hw_config_reads, 1);
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
    mock_fault(ADDR_HARDWARE_CONFIG, 24, 0, 1);
    const alltrax_var_def* var = alltrax_find_var("Throttle_Type");
    double value = 0;
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, &value, 1, NULL), ALLTRAX_OK);
    ASSERT_EQ(mock.hw_config_reads, 0);
    ASSERT_EQ(mock.page[0xA0], 0);
    ASSERT_EQ(mock.erases, 1);
    ASSERT_EQ(mock.mode_writes, 2);
    ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    return 0;
}

static int test_repeated_and_overlapping_targets_reject_batch_before_io(void)
{
    const alltrax_curve_def* lin = alltrax_find_curve("linearization");
    alltrax_curve_data zero = { .def = lin };
    const alltrax_var_def* type = alltrax_find_var("Throttle_Type");
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    const alltrax_assignment cases[][2] = {
        { { .var = type, .number = 0 }, { .var = type, .number = 1 } },
        { { .var = hpd, .number = 1 }, { .var = hpd, .number = 1 } },
        { { .curve = lin, .curve_data = &zero }, { .curve = lin, .curve_data = &zero } },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mock_reset();
        ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, cases[i], 2, &batch_opts),
                  ALLTRAX_ERR_INVALID_ARG);
        ASSERT_TRUE(strstr(mock.ctrl.error_detail, "once per batch") != NULL);
        ASSERT_EQ(mock.requests, 0);
    }
    return 0;
}

static int test_voltage_settings_bounded_by_rated_voltage(void)
{
    const struct { uint16_t rated; const char* name; double volts; bool skip_link; bool ok; } cases[] = {
        { 48, "LoBat_Vlim", 16.0, true, true },
        { 48, "LoBat_Vlim", 15.9, true, false },
        { 48, "HiBat_Vlim", 62.0, true, true },
        { 48, "HiBat_Vlim", 62.1, true, false },
        { 48, "AnalogInputs_Threshold", 15.9, true, false },
        { 72, "LoBat_Vlim", 19.9, true, false },
        { 72, "HiBat_Vlim", 90.0, true, true },
        { 72, "HiBat_Vlim", 90.1, true, false },
        { 36, "LoBat_Vlim", 30.0, true, false },
        { 48, "BMS_Missing_HiBat_Vlim", 70.0, true, true },
        { 48, "BMS_Missing_HiBat_Vlim", 70.0, false, false },
        { 36, "BMS_Missing_HiBat_Vlim", 30.0, true, true },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mock_reset();
        mock.rated_volts = cases[i].rated;
        /* Existing limits that keep the link check satisfied for the cases
         * that write only the variable under test. */
        const struct { uint32_t addr; uint16_t raw; } page[] = {
            { 0x08002002, 160 }, { 0x08002020, 170 },
            { 0x08002022, 620 }, { 0x08002026, 600 },
        };
        for (size_t p = 0; p < 4; p++) {
            mock.page[page[p].addr - VARSET_ADDR] = (uint8_t)page[p].raw;
            mock.page[page[p].addr - VARSET_ADDR + 1] = (uint8_t)(page[p].raw >> 8);
        }
        const alltrax_var_def* var = alltrax_find_var(cases[i].name);
        alltrax_write_opts opts = { .skip_voltage_link = cases[i].skip_link };
        alltrax_error rc = alltrax_write_vars(&mock.ctrl, &var, &cases[i].volts, 1, &opts);
        if (cases[i].ok) {
            ASSERT_EQ(rc, ALLTRAX_OK);
        } else {
            ASSERT_EQ(rc, ALLTRAX_ERR_INVALID_ARG);
            ASSERT_TRUE(strstr(mock.ctrl.error_detail, "V") != NULL);
            ASSERT_EQ(mock.mode_writes, 0);
            ASSERT_EQ(mock.erases, 0);
        }
    }
    return 0;
}

static int test_old_firmware_throttle_default_excludes_ezgo(void)
{
    const alltrax_var_def* var = alltrax_find_var("Throttle_Type");
    for (double type = 1; type <= 11; type++) {
        mock_reset();
        mock.orig_boot_rev = mock.orig_program_rev = 1;
        mock.throttles_allowed = 0;  /* Unprogrammed bits are ignored. */
        ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, &type, 1, NULL),
                  type == 4 ? ALLTRAX_ERR_INVALID_ARG : ALLTRAX_OK);
    }
    return 0;
}

static int test_throttle_mask_uses_encoded_value(void)
{
    const alltrax_var_def* var = alltrax_find_var("Throttle_Type");
    double value = 2;
    mock_reset();
    mock.throttles_allowed = 0x001;  /* Only type 1 */
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, &value, 1, NULL), ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(mock.mode_writes, 0);
    ASSERT_EQ(mock.erases, 0);
    mock_reset();
    mock.throttles_allowed = 0x002;  /* Type 2 maps to bit 1 */
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, &value, 1, NULL), ALLTRAX_OK);
    ASSERT_EQ(mock.page[var->address - VARSET_ADDR], 2);
    return 0;
}

static int test_all_canonical_variable_groups_remain_readable(void)
{
    mock_reset();
    for (int g = 0; g < ALLTRAX_VARS_GROUP_COUNT; g++) {
        size_t count;
        const alltrax_var_def* defs = alltrax_get_var_defs((alltrax_var_group)g, &count);
        ASSERT_TRUE(count <= 64);
        const alltrax_var_def* vars[64];
        alltrax_var_value out[64];
        for (size_t i = 0; i < count; i++) vars[i] = &defs[i];
        ASSERT_EQ(alltrax_read_vars(&mock.ctrl, vars, count, out), ALLTRAX_OK);
    }
    return 0;
}

/* Exercise both variable paths and the separate curve entry point. */
static alltrax_error mode_test_write(int operation, const alltrax_write_opts* opts)
{
    if (operation < 2) {
        const alltrax_var_def* var = alltrax_find_var(operation == 0 ? "Fan_On" : "HPD");
        double value = 1;
        return alltrax_write_vars(&mock.ctrl, &var, &value, 1, opts);
    }
    const alltrax_curve_def* def = alltrax_find_curve("linearization");
    const alltrax_curve_data data = { .def = def };
    return alltrax_write_curve(&mock.ctrl, def, &data, opts);
}

static int test_failed_cal_entry_attempts_run_and_skips_requested_writes(void)
{
    const alltrax_error expected[] = {
        ALLTRAX_OK, ALLTRAX_ERR_TIMEOUT, ALLTRAX_ERR_USB, ALLTRAX_ERR_PROTOCOL,
        ALLTRAX_ERR_VERIFY, ALLTRAX_ERR_VERIFY, ALLTRAX_ERR_DEVICE_FAIL,
    };
    for (int op = 0; op < 3; op++) {
        for (int fault = 1; fault <= 7; fault++) {
            mock_reset();
            uint8_t original[VARSET_SIZE];
            memcpy(original, mock.page, sizeof(original));
            if (fault == 7) mock.cal_transfer_error = true;
            else mock.cal_reply = (mode_reply_fault)fault;
            ASSERT_EQ(mode_test_write(op, NULL), fault == 7 ? ALLTRAX_ERR_USB : expected[fault]);
            ASSERT_EQ(mock.cal_attempts, 1);
            ASSERT_EQ(mock.run_attempts, 1);
            ASSERT_EQ(mock.mode, RUN_MODE_RUN);
            ASSERT_EQ(mock.fan_writes, 0);
            ASSERT_EQ(mock.settings_writes, 0);
            ASSERT_EQ(mock.erases, 0);
            ASSERT_MEM_EQ(mock.page, original, sizeof(original));
            if (fault == MODE_REPLY_TIMEOUT)
                ASSERT_STR_EQ(mock.ctrl.error_detail, "CAL acknowledgement timeout");
            if (fault == 7) ASSERT_STR_EQ(mock.ctrl.error_detail, "CAL transfer failure");
        }
    }
    return 0;
}

static int test_failures_before_cal_attempt_do_not_write_run(void)
{
    for (int op = 0; op < 3; op++) {
        for (int failure = 0; failure < 3; failure++) {
            mock_reset();
            if (failure == 0) mock_fault(ADDR_RUN_MODE, 1, 0, 1);
            else if (failure == 1) mock.mode = RUN_MODE_CAL;
            else mock.cal_write_blocked = true;
            ASSERT_EQ(mode_test_write(op, NULL),
                      failure == 0 ? ALLTRAX_ERR_PROTOCOL :
                      failure == 1 ? ALLTRAX_ERR_NOT_RUN : ALLTRAX_ERR_BLOCKED);
            ASSERT_EQ(mock.cal_attempts, 0);
            ASSERT_EQ(mock.run_attempts, 0);
            ASSERT_EQ(mock.mode_writes, 0);
            ASSERT_EQ(mock.mode, failure == 1 ? RUN_MODE_CAL : RUN_MODE_RUN);
        }
    }
    return 0;
}

static int test_cal_and_run_failures_preserve_both_diagnostics(void)
{
    for (int op = 0; op < 3; op++) {
        mock_reset();
        mock.cal_reply = MODE_REPLY_TIMEOUT;
        mock.run_transfer_error = true;
        ASSERT_EQ(mode_test_write(op, NULL), ALLTRAX_ERR_TIMEOUT);
        ASSERT_EQ(mock.run_attempts, 1);
        ASSERT_EQ(mock.mode, RUN_MODE_CAL);
        ASSERT_TRUE(strstr(mock.ctrl.error_detail, "CAL acknowledgement timeout") != NULL);
        ASSERT_TRUE(strstr(mock.ctrl.error_detail, "RUN restoration failed (-1)") != NULL);
        ASSERT_TRUE(strstr(mock.ctrl.error_detail, "RUN transfer failure") != NULL);
        mock_reset();
        mock.cal_transfer_error = true;
        mock.run_reply = MODE_REPLY_TIMEOUT;
        ASSERT_EQ(mode_test_write(op, NULL), ALLTRAX_ERR_USB);
        ASSERT_TRUE(strstr(mock.ctrl.error_detail, "CAL transfer failure") != NULL);
        ASSERT_TRUE(strstr(mock.ctrl.error_detail, "RUN restoration failed (-2)") != NULL);
        ASSERT_TRUE(strstr(mock.ctrl.error_detail, "RUN acknowledgement timeout") != NULL);
    }
    return 0;
}

static int test_body_and_run_failures_preserve_original_error(void)
{
    for (int op = 0; op < 3; op++) {
        for (int cleanup_failure = 0; cleanup_failure < 2; cleanup_failure++) {
            mock_reset();
            if (op == 0) mock.fan_transfer_error = true;
            else mock_fault(VARSET_ADDR, MAX_PAYLOAD, 0, 1);
            if (cleanup_failure) mock.run_reply = MODE_REPLY_TIMEOUT;
            ASSERT_EQ(mode_test_write(op, NULL), op == 0 ? ALLTRAX_ERR_USB : ALLTRAX_ERR_PROTOCOL);
            ASSERT_EQ(mock.cal_attempts, 1);
            ASSERT_EQ(mock.run_attempts, 1);
            ASSERT_TRUE(strstr(mock.ctrl.error_detail,
                               op == 0 ? "Fan_On transfer failure" : "Read length mismatch") != NULL);
            if (cleanup_failure) {
                ASSERT_TRUE(strstr(mock.ctrl.error_detail, "RUN restoration failed (-2)") != NULL);
                ASSERT_TRUE(strstr(mock.ctrl.error_detail, "RUN acknowledgement timeout") != NULL);
            }
            ASSERT_EQ(mock.erases, 0);
        }
    }
    return 0;
}

static int test_run_failure_after_success_is_reported_explicitly(void)
{
    for (int op = 0; op < 3; op++) {
        for (int transfer_failure = 0; transfer_failure < 2; transfer_failure++) {
            mock_reset();
            if (transfer_failure) mock.run_transfer_error = true;
            else mock.run_reply = MODE_REPLY_TIMEOUT;
            ASSERT_EQ(mode_test_write(op, NULL), transfer_failure ? ALLTRAX_ERR_USB : ALLTRAX_ERR_TIMEOUT);
            ASSERT_EQ(mock.run_attempts, 1);
            ASSERT_TRUE(strstr(mock.ctrl.error_detail, "RUN restoration failed") != NULL);
            ASSERT_EQ(mock.mode, transfer_failure ? RUN_MODE_CAL : RUN_MODE_RUN);
            ASSERT_EQ(mock.fan_writes, op == 0 ? 1 : 0);
            ASSERT_EQ(mock.erases, op == 0 ? 0 : 1);
        }
    }
    return 0;
}

static int test_skip_cal_does_not_introduce_mode_cleanup(void)
{
    const alltrax_write_opts opts = { .skip_cal = true };
    for (int op = 0; op < 3; op++) {
        mock_reset();
        ASSERT_EQ(mode_test_write(op, &opts), ALLTRAX_OK);
        ASSERT_EQ(mock.mode_writes, 0);
        mock_reset();
        if (op == 0) mock.fan_transfer_error = true;
        else mock_fault(VARSET_ADDR, MAX_PAYLOAD, 0, 1);
        ASSERT_EQ(mode_test_write(op, &opts), op == 0 ? ALLTRAX_ERR_USB : ALLTRAX_ERR_PROTOCOL);
        ASSERT_EQ(mock.mode_writes, 0);
    }
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

static void set_firmware(uint32_t boot_rev, uint32_t program_rev)
{
    mock.ctrl.boot_rev = mock.boot_rev = boot_rev;
    mock.ctrl.program_rev = mock.program_rev = program_rev;
}

/* Returns the first public read result that differs from expected. */
static alltrax_error read_operations(alltrax_error expected)
{
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    const alltrax_curve_def* curve = alltrax_find_curve("linearization");
    alltrax_var_value value;
    alltrax_curve_data data;
    alltrax_monitor_data monitor;
    alltrax_error rc[] = {
        alltrax_read_vars(&mock.ctrl, &hpd, 1, &value),
        alltrax_read_curve(&mock.ctrl, curve, &data),
        alltrax_read_curve_factory(&mock.ctrl, curve, &data),
        alltrax_read_monitor(&mock.ctrl, &monitor),
    };
    for (size_t i = 0; i < sizeof(rc) / sizeof(rc[0]); i++) {
        if (rc[i] != expected) return rc[i];
    }
    return expected;
}

/* Returns the first RAM, FLASH, curve, or reset result that differs from
 * expected. */
static alltrax_error write_operations(const alltrax_write_opts* opts,
    alltrax_error expected)
{
    const alltrax_var_def* fan = alltrax_find_var("Fan_On");
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    const alltrax_curve_def* curve = alltrax_find_curve("linearization");
    const alltrax_curve_preset* preset =
        alltrax_find_curve_preset("linearization", "linear");
    alltrax_curve_data data = { .def = curve };
    memcpy(data.x, preset->x, sizeof(data.x));
    memcpy(data.y, preset->y, sizeof(data.y));
    double one = 1;
    alltrax_error rc[] = {
        alltrax_write_vars(&mock.ctrl, &fan, &one, 1, opts),
        alltrax_write_vars(&mock.ctrl, &hpd, &one, 1, opts),
        alltrax_write_curve(&mock.ctrl, curve, &data, opts),
        alltrax_reset_device(&mock.ctrl, opts),
    };
    for (size_t i = 0; i < sizeof(rc) / sizeof(rc[0]); i++) {
        if (rc[i] != expected) return rc[i];
    }
    return expected;
}

static int test_firmware_policy_tiers(void)
{
    const struct {
        uint32_t boot, program;
        bool read, write, write_with_skip;
    } cases[] = {
        { 5002, 5005,       true,  true,  true  },
        { 5002, 5004,       true,  false, true  },
        { 5002, 1,          true,  false, true  },
        { 5999, 5999,       true,  false, true  },
        { 5002, 6000,       false, false, false },
        { 6000, 5005,       false, false, false },
        { 5002, 0,          false, false, false },
        { 5002, 0xFFFFFFFF, false, false, false },
    };
    const alltrax_write_opts skip = { .skip_fw_check = true };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mock_reset();
        set_firmware(cases[i].boot, cases[i].program);
        uint8_t original[VARSET_SIZE];
        memcpy(original, mock.page, sizeof(original));

        alltrax_error expected = cases[i].read ? ALLTRAX_OK : ALLTRAX_ERR_FIRMWARE;
        ASSERT_EQ(read_operations(expected), expected);
        if (!cases[i].read)
            ASSERT_EQ(mock.requests, 0);

        unsigned before = mock.requests;
        expected = cases[i].write ? ALLTRAX_OK : ALLTRAX_ERR_FIRMWARE;
        ASSERT_EQ(write_operations(NULL, expected), expected);
        if (!cases[i].write) {
            ASSERT_EQ(mock.requests, before);
            ASSERT_MEM_EQ(mock.page, original, sizeof(original));
        }

        before = mock.requests;
        expected = cases[i].write_with_skip ? ALLTRAX_OK : ALLTRAX_ERR_FIRMWARE;
        ASSERT_EQ(write_operations(&skip, expected), expected);
        if (!cases[i].write_with_skip)
            ASSERT_EQ(mock.requests, before);
        ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    }
    return 0;
}

static int test_firmware_read_override_does_not_enable_writes(void)
{
    mock_reset();
    set_firmware(5002, 6001);
    ASSERT_EQ(alltrax_set_firmware_read_override(NULL, true), ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(read_operations(ALLTRAX_ERR_FIRMWARE), ALLTRAX_ERR_FIRMWARE);
    ASSERT_TRUE(strstr(mock.ctrl.error_detail, "V6.001") != NULL);
    ASSERT_EQ(mock.requests, 0);

    ASSERT_EQ(alltrax_set_firmware_read_override(&mock.ctrl, true), ALLTRAX_OK);
    ASSERT_EQ(read_operations(ALLTRAX_OK), ALLTRAX_OK);

    unsigned before = mock.requests;
    const alltrax_write_opts skip = { .skip_fw_check = true };
    ASSERT_EQ(write_operations(&skip, ALLTRAX_ERR_FIRMWARE), ALLTRAX_ERR_FIRMWARE);
    ASSERT_TRUE(strstr(mock.ctrl.error_detail, "writes are not allowed") != NULL);
    ASSERT_EQ(mock.requests, before);

    ASSERT_EQ(alltrax_set_firmware_read_override(&mock.ctrl, false), ALLTRAX_OK);
    ASSERT_EQ(read_operations(ALLTRAX_ERR_FIRMWARE), ALLTRAX_ERR_FIRMWARE);
    return 0;
}

static int test_unvalidated_write_diagnostic_names_firmware(void)
{
    mock_reset();
    set_firmware(5002, 5004);
    const alltrax_var_def* fan = alltrax_find_var("Fan_On");
    double one = 1;
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &fan, &one, 1, NULL), ALLTRAX_ERR_FIRMWARE);
    ASSERT_TRUE(strstr(mock.ctrl.error_detail, "V5.004 is not validated") != NULL);
    ASSERT_EQ(mock.requests, 0);
    return 0;
}

static int test_info_reports_policy_and_refreshes_revisions(void)
{
    const struct {
        uint32_t program;
        bool supported, write_validated;
    } cases[] = {
        { 5005, true,  true  },
        { 5004, true,  false },
        { 6001, false, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mock_reset();
        mock.program_rev = cases[i].program;  /* Device differs from cache. */
        alltrax_info info;
        ASSERT_EQ(alltrax_get_info(&mock.ctrl, &info), ALLTRAX_OK);
        ASSERT_EQ(info.program_rev, cases[i].program);
        ASSERT_EQ(info.supported, cases[i].supported);
        ASSERT_EQ(info.write_validated, cases[i].write_validated);
        ASSERT_EQ(mock.ctrl.program_rev, cases[i].program);
        alltrax_error expected = cases[i].supported ? ALLTRAX_OK : ALLTRAX_ERR_FIRMWARE;
        ASSERT_EQ(read_operations(expected), expected);
    }
    return 0;
}

static void put_monitor_le16(uint32_t addr, int16_t value)
{
    mock.monitor_ram[addr - 0x2000F000] = (uint8_t)value;
    mock.monitor_ram[addr - 0x2000F000 + 1] = (uint8_t)((uint16_t)value >> 8);
}

static int test_monitor_reads_user_profile_and_inputs(void)
{
    mock_reset();
    put_monitor_le16(0x2000F160, 2);     /* UserProfile_Num */
    put_monitor_le16(0x2000F162, -1);    /* User1_InputPercent */
    put_monitor_le16(0x2000F164, 75);    /* User2_InputPercent */
    put_monitor_le16(0x2000F13A, 3210);  /* Speed */
    put_monitor_le16(0x2000F14A, 875);   /* State_Of_Charge, 0.1 % */
    put_monitor_le16(0x2000F15A, -123);  /* Battery_Amps low word, 0.1 A */
    put_monitor_le16(0x2000F15C, -1);    /* Battery_Amps high word */
    mock.monitor_ram[0x09C] = 1;         /* User3_Input */

    alltrax_monitor_data data;
    ASSERT_EQ(alltrax_read_monitor(&mock.ctrl, &data), ALLTRAX_OK);
    ASSERT_EQ(mock.requests, 6);
    ASSERT_EQ(data.user_profile_num, 2);
    ASSERT_EQ(data.user1_input_percent, -1);
    ASSERT_EQ(data.user2_input_percent, 75);
    ASSERT_EQ(data.speed_rpm, 3210);
    ASSERT_TRUE(fabs(data.state_of_charge - 87.5) < 1e-9);
    ASSERT_TRUE(fabs(data.battery_amps - -12.3) < 1e-9);
    ASSERT_TRUE(data.user3_input);
    ASSERT_FALSE(data.user1_input);
    return 0;
}


static int test_string_writes_are_space_padded(void)
{
    const struct { const char* name; const char* text; const char* stored; } cases[] = {
        { "F_Table_Name",       "MY MAP",           "MY MAP" },
        { "F_Table_Name",       "",                 "" },
        { "F_Table_Name",       "TRAILING   ",      "TRAILING" },
        { "Throttle_Type_Name", "ABCDEFGHIJKLMNOP", "ABCDEFGHIJKLMNOP" },
        { "Throttle_Type_Name", "~ !\"#",           "~ !\"#" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mock_reset();
        const alltrax_var_def* var = alltrax_find_var(cases[i].name);
        size_t offset = var->address - VARSET_ADDR;
        uint8_t expected[VARSET_SIZE];
        memcpy(expected, mock.page, sizeof(expected));
        memset(expected + offset, ' ', var->size);
        memcpy(expected + offset, cases[i].stored, strlen(cases[i].stored));

        alltrax_assignment item = { .var = var, .text = cases[i].text };
        ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, &item, 1, &batch_opts),
                  ALLTRAX_OK);
        ASSERT_MEM_EQ(mock.page, expected, sizeof(expected));
        ASSERT_EQ(mock.erases, 1);

        /* Reading back trims the padding. */
        alltrax_var_value value;
        ASSERT_EQ(alltrax_read_vars(&mock.ctrl, &var, 1, &value), ALLTRAX_OK);
        ASSERT_STR_EQ(value.raw.str, cases[i].stored);
    }
    return 0;
}

static int test_invalid_string_writes_rejected_before_io(void)
{
    const struct { const char* name; const char* text; } bad[] = {
        { "Throttle_Type_Name", "ABCDEFGHIJKLMNOPQ" },  /* 17 > 16 */
        { "F_Table_Name", "caf\xc3\xa9" },              /* UTF-8 */
        { "F_Table_Name", "tab\there" },
        { "F_Table_Name", "\x7f" },
        { "F_Table_Name", NULL },                       /* string without text */
        { "F_F_Table_Name", "FACTORY" },                /* read-only */
        { "Model", "XCT" },                             /* read-only */
        { "HPD", "1" },                                 /* text on a bool */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        mock_reset();
        uint8_t original[VARSET_SIZE];
        memcpy(original, mock.page, sizeof(original));
        alltrax_assignment items[] = {
            { .var = alltrax_find_var("HPD"), .number = 1 },
            { .var = alltrax_find_var(bad[i].name), .text = bad[i].text },
        };
        ASSERT_TRUE(items[1].var != NULL);
        ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, items, 2, &batch_opts),
                  ALLTRAX_ERR_INVALID_ARG);
        ASSERT_TRUE(strstr(mock.ctrl.error_detail, bad[i].name) != NULL);
        ASSERT_EQ(mock.requests, 0);
        ASSERT_MEM_EQ(mock.page, original, sizeof(original));
    }
    ASSERT_EQ(alltrax_validate_var_string(alltrax_find_var("F_Table_Name"), "OK"),
              ALLTRAX_OK);
    ASSERT_EQ(alltrax_validate_var_string(alltrax_find_var("HPD"), "1"),
              ALLTRAX_ERR_INVALID_ARG);
    return 0;
}

static int test_mixed_batch_uses_one_bracket_and_one_erase(void)
{
    mock_reset();
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    const alltrax_var_def* name = alltrax_find_var("F_Table_Name");
    const alltrax_curve_def* lin = alltrax_find_curve("linearization");
    const alltrax_curve_def* torque = alltrax_find_curve("torque");
    alltrax_curve_data lin_data = { .def = lin }, torque_data = { .def = torque };
    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++) {
        lin_data.x[i] = lin_data.y[i] = i * 6.0;
        torque_data.x[i] = torque_data.y[i] = i * 5.0;
    }
    alltrax_assignment items[] = {
        { .var = alltrax_find_var("Fan_On"), .number = 1 },
        { .var = hpd, .number = 1 },
        { .curve = lin, .curve_data = &lin_data },
        { .var = name, .text = "ROUND TRIP" },
        { .curve = torque, .curve_data = &torque_data },
    };

    uint8_t expected[VARSET_SIZE];
    memcpy(expected, mock.page, sizeof(expected));
    expected[hpd->address - VARSET_ADDR] = 1;
    memset(expected + (name->address - VARSET_ADDR), ' ', name->size);
    memcpy(expected + (name->address - VARSET_ADDR), "ROUND TRIP", 10);
    ASSERT_EQ(encode_curve_array(lin_data.x, lin->x_scale, lin->x_raw_max,
                                 expected + (lin->x_address - VARSET_ADDR)), ALLTRAX_OK);
    ASSERT_EQ(encode_curve_array(lin_data.y, lin->y_scale, lin->y_raw_max,
                                 expected + (lin->y_address - VARSET_ADDR)), ALLTRAX_OK);
    ASSERT_EQ(encode_curve_array(torque_data.x, torque->x_scale, torque->x_raw_max,
                                 expected + (torque->x_address - VARSET_ADDR)), ALLTRAX_OK);
    ASSERT_EQ(encode_curve_array(torque_data.y, torque->y_scale, torque->y_raw_max,
                                 expected + (torque->y_address - VARSET_ADDR)), ALLTRAX_OK);

    ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, items, 5, &batch_opts), ALLTRAX_OK);
    ASSERT_EQ(mock.erases, 1);
    ASSERT_EQ(mock.cal_attempts, 1);
    ASSERT_EQ(mock.run_attempts, 1);
    ASSERT_EQ(mock.fan_writes, 1);
    ASSERT_EQ(mock.goodset_clears, 1);
    ASSERT_MEM_EQ(mock.page, expected, sizeof(expected));
    ASSERT_EQ(mock.mode, RUN_MODE_RUN);
    return 0;
}

static int test_batch_rejects_bad_items_without_io(void)
{
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    const alltrax_curve_def* lin = alltrax_find_curve("linearization");
    alltrax_curve_data bad_curve = { .def = lin };
    bad_curve.x[3] = NAN;
    const alltrax_assignment bad[] = {
        { .number = 1 },                                   /* neither */
        { .var = hpd, .number = 1, .curve = lin },         /* both */
        { .curve = lin },                                  /* no data */
        { .curve = lin, .curve_data = &bad_curve },        /* invalid point */
        { .var = alltrax_find_var("N_Speed_Limit"), .number = 9000 }, /* range */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        mock_reset();
        const alltrax_assignment items[] = {
            { .var = alltrax_find_var("F_Table_Name"), .text = "OK" }, bad[i],
        };
        ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, items, 2, &batch_opts),
                  ALLTRAX_ERR_INVALID_ARG);
        ASSERT_EQ(mock.requests, 0);
    }
    mock_reset();
    ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, NULL, 1, &batch_opts),
              ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, bad, 0, &batch_opts),
              ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(mock.requests, 0);
    return 0;
}

static int test_batch_flash_patch_limit(void)
{
    alltrax_assignment items[97];
    for (size_t i = 0; i < 97; i++)
        items[i] = (alltrax_assignment){ .var = alltrax_find_var("HPD"), .number = 1 };
    mock_reset();
    ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, items, 97, &batch_opts),
              ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(mock.requests, 0);

    /* Each curve uses two patches, so 49 curves exceed the limit. */
    const alltrax_curve_def* lin = alltrax_find_curve("linearization");
    alltrax_curve_data zero = { .def = lin };
    for (size_t i = 0; i < 49; i++)
        items[i] = (alltrax_assignment){ .curve = lin, .curve_data = &zero };
    mock_reset();
    ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, items, 49, &batch_opts),
              ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(mock.requests, 0);

    /* Every writable settings variable and curve fits in one batch. */
    alltrax_curve_data curves[8];
    size_t n = 0;
    for (int g = 0; g < ALLTRAX_VARS_GROUP_COUNT; g++) {
        size_t count;
        const alltrax_var_def* defs = alltrax_get_var_defs((alltrax_var_group)g, &count);
        for (size_t i = 0; i < count; i++) {
            const alltrax_var_def* var = &defs[i];
            if (!var->writable || !var->is_flash)
                continue;
            double min, max;
            alltrax_var_display_bounds(var, &min, &max);
            int v = strcmp(var->name, "AnalogInputs_Threshold") == 0 ? 0 :
                    strcmp(var->name, "LoBat_Vlim") == 0 ? 1 :
                    strcmp(var->name, "HiBat_Vlim") == 0 ? 2 : -1;
            items[n++] = var->type == ALLTRAX_TYPE_STRING
                ? (alltrax_assignment){ .var = var, .text = "" }
                : (alltrax_assignment){ .var = var,
                      .number = v >= 0 ? 20.0 + 10 * v : isnan(min) ? 0 : min };
        }
    }
    for (size_t c = 0; c < alltrax_curve_count(); c++) {
        curves[c] = (alltrax_curve_data){ .def = alltrax_curve_by_index(c) };
        items[n++] = (alltrax_assignment){ .curve = curves[c].def, .curve_data = &curves[c] };
    }
    mock_reset();
    ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, items, n, &batch_opts), ALLTRAX_OK);
    ASSERT_EQ(mock.erases, 1);
    return 0;
}

static int test_batch_keeps_voltage_link_checks(void)
{
    mock_reset();
    alltrax_assignment items[] = {
        { .var = alltrax_find_var("F_Table_Name"), .text = "MAP" },
        { .var = alltrax_find_var("AnalogInputs_Threshold"), .number = 30.0 },
        { .var = alltrax_find_var("LoBat_Vlim"), .number = 20.0 },
        { .var = alltrax_find_var("HiBat_Vlim"), .number = 60.0 },
        { .var = alltrax_find_var("BMS_Missing_HiBat_Vlim"), .number = 55.0 },
    };
    ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, items, 5, NULL),
              ALLTRAX_ERR_INVALID_ARG);
    ASSERT_TRUE(strstr(mock.ctrl.error_detail, "KSI") != NULL);
    ASSERT_EQ(mock.mode_writes, 0);
    ASSERT_EQ(mock.erases, 0);
    return 0;
}

static int test_flash_verification_mismatch_keeps_goodset_invalid(void)
{
    mock_reset();
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    size_t offset = hpd->address - VARSET_ADDR;
    mock.stuck_offset = offset + 1;
    double one = 1;
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &hpd, &one, 1, NULL), ALLTRAX_ERR_FLASH);
    char text[64];
    snprintf(text, sizeof(text), "verification failed at offset 0x%04X", (unsigned)offset);
    ASSERT_TRUE(strstr(mock.ctrl.error_detail, text) != NULL);
    ASSERT_EQ(mock.erases, 1);
    ASSERT_EQ(mock.goodset_clears, 0);
    ASSERT_EQ(get_le16(mock.page), 0xFFFF);
    ASSERT_EQ(mock.run_attempts, 1);
    ASSERT_EQ(mock.mode, RUN_MODE_RUN);

    /* skip_verify omits the read-back, so the mismatch goes undetected. */
    mock_reset();
    mock.stuck_offset = offset + 1;
    const alltrax_write_opts no_verify = { .skip_verify = true };
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &hpd, &one, 1, &no_verify), ALLTRAX_OK);
    ASSERT_EQ(mock.goodset_clears, 1);
    return 0;
}

static int test_invalid_goodset_blocks_writes_before_cal(void)
{
    mock_reset();
    mock.page[0] = mock.page[1] = 0xFF;  /* Interrupted earlier write */
    uint8_t original[VARSET_SIZE];
    memcpy(original, mock.page, sizeof(original));
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    double one = 1;
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &hpd, &one, 1, NULL), ALLTRAX_ERR_FLASH);
    ASSERT_TRUE(strstr(mock.ctrl.error_detail, "V_GoodSet=0xFFFF") != NULL);
    ASSERT_EQ(mock.mode_writes, 0);
    ASSERT_EQ(mock.erases, 0);
    ASSERT_EQ(mock.settings_writes, 0);
    ASSERT_MEM_EQ(mock.page, original, sizeof(original));

    const alltrax_write_opts skip = { .skip_goodset = true };
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &hpd, &one, 1, &skip), ALLTRAX_OK);
    ASSERT_EQ(mock.erases, 1);
    ASSERT_EQ(mock.goodset_clears, 1);
    ASSERT_EQ(get_le16(mock.page), 0x0000);
    ASSERT_EQ(mock.page[hpd->address - VARSET_ADDR], 1);
    return 0;
}

/* Caller-built descriptors, including unmodified copies of library entries,
 * never reach the device. */
static int test_non_catalog_variables_rejected_before_io(void)
{
    alltrax_var_def copies[8];
    for (size_t i = 0; i < 8; i++)
        copies[i] = *alltrax_find_var(i % 2 ? "HPD" : "Fan_On");
    copies[1].address = VARSET_ADDR + 0x7F0;                 /* no variable */
    copies[2].address = 0x20000100;                          /* arbitrary RAM */
    copies[3] = *alltrax_find_var("Global_Shutdown");
    copies[3].writable = true;                               /* read-only */
    copies[4].type = ALLTRAX_TYPE_UINT32;
    copies[4].address = RAM_END - 1;                         /* crosses RAM end */
    copies[5].name = NULL;
    copies[6].type = (alltrax_var_type)99;
    copies[7].is_flash = true;
    copies[7].address = 0x08004000;                          /* firmware area */
    const alltrax_write_opts unchecked = {
        .skip_cal = true, .skip_verify = true, .skip_goodset = true,
        .skip_fw_check = true, .skip_voltage_link = true,
    };
    for (size_t i = 0; i <= 8; i++) {
        const alltrax_var_def* bad = i < 8 ? &copies[i] : NULL;
        const alltrax_var_def* vars[] = { alltrax_find_var("HPD"), bad };
        const double values[] = { 1, 1 };
        mock_reset();
        uint8_t original[VARSET_SIZE];
        memcpy(original, mock.page, sizeof(original));
        ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars, values, 2, NULL), ALLTRAX_ERR_INVALID_ARG);
        if (bad)  /* A NULL entry reports a missing variable instead. */
            ASSERT_TRUE(strstr(mock.ctrl.error_detail, "Not a library variable") != NULL);
        ASSERT_EQ(alltrax_write_vars(&mock.ctrl, vars, values, 2, &unchecked), ALLTRAX_ERR_INVALID_ARG);
        const alltrax_assignment item = { .var = bad, .number = 1 };
        ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, &item, 1, &unchecked), ALLTRAX_ERR_INVALID_ARG);
        alltrax_var_value out[2] = {{0}}, before[2];
        memcpy(before, out, sizeof(out));
        ASSERT_EQ(alltrax_read_vars(&mock.ctrl, vars, 2, out), ALLTRAX_ERR_INVALID_ARG);
        ASSERT_MEM_EQ(out, before, sizeof(out));
        ASSERT_EQ(mock.requests, 0);
        ASSERT_MEM_EQ(mock.page, original, sizeof(original));
    }

    mock_reset();
    const alltrax_var_def* var = alltrax_find_var("HPD");
    double value = 1;
    alltrax_var_value out;
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, NULL, &value, 1, NULL), ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(alltrax_write_vars(&mock.ctrl, &var, NULL, 1, NULL), ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(alltrax_read_vars(&mock.ctrl, NULL, 1, &out), ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(alltrax_read_vars(&mock.ctrl, &var, 1, NULL), ALLTRAX_ERR_INVALID_ARG);
    ASSERT_EQ(mock.requests, 0);
    return 0;
}

static int test_non_catalog_curves_rejected_before_io(void)
{
    const alltrax_curve_def* lin = alltrax_find_curve("linearization");
    alltrax_curve_def copies[4] = { *lin, *lin, *lin, *lin };
    copies[1].x_address = UINT32_MAX - 15;            /* wraps */
    copies[2].y_address = VARSET_ADDR + VARSET_SIZE - 31;
    copies[3].x_scale = 0;
    for (size_t i = 0; i <= 4; i++) {
        const alltrax_curve_def* bad = i < 4 ? &copies[i] : NULL;
        mock_reset();
        alltrax_curve_data data = { .def = lin }, before;
        memcpy(&before, &data, sizeof(data));
        ASSERT_EQ(alltrax_read_curve(&mock.ctrl, bad, &data), ALLTRAX_ERR_INVALID_ARG);
        ASSERT_EQ(alltrax_read_curve_factory(&mock.ctrl, bad, &data), ALLTRAX_ERR_INVALID_ARG);
        ASSERT_MEM_EQ(&data, &before, sizeof(data));
        ASSERT_EQ(alltrax_validate_curve(bad, &data), ALLTRAX_ERR_INVALID_ARG);
        ASSERT_EQ(alltrax_write_curve(&mock.ctrl, bad, &data, NULL), ALLTRAX_ERR_INVALID_ARG);
        const alltrax_assignment item = { .curve = bad, .curve_data = &data };
        ASSERT_EQ(alltrax_write_assignments(&mock.ctrl, &item, 1, NULL), ALLTRAX_ERR_INVALID_ARG);
        ASSERT_EQ(mock.requests, 0);
        ASSERT_EQ(mock.erases, 0);
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
    RUN_TEST(test_repeated_and_overlapping_targets_reject_batch_before_io);
    RUN_TEST(test_old_firmware_throttle_default_excludes_ezgo);
    RUN_TEST(test_voltage_settings_bounded_by_rated_voltage);
    RUN_TEST(test_throttle_mask_uses_encoded_value);
    RUN_TEST(test_non_catalog_variables_rejected_before_io);
    RUN_TEST(test_non_catalog_curves_rejected_before_io);
    RUN_TEST(test_all_canonical_variable_groups_remain_readable);
    RUN_TEST(test_failed_cal_entry_attempts_run_and_skips_requested_writes);
    RUN_TEST(test_failures_before_cal_attempt_do_not_write_run);
    RUN_TEST(test_cal_and_run_failures_preserve_both_diagnostics);
    RUN_TEST(test_body_and_run_failures_preserve_original_error);
    RUN_TEST(test_run_failure_after_success_is_reported_explicitly);
    RUN_TEST(test_skip_cal_does_not_introduce_mode_cleanup);
    RUN_TEST(test_firmware_policy_tiers);
    RUN_TEST(test_firmware_read_override_does_not_enable_writes);
    RUN_TEST(test_unvalidated_write_diagnostic_names_firmware);
    RUN_TEST(test_info_reports_policy_and_refreshes_revisions);
    RUN_TEST(test_monitor_reads_user_profile_and_inputs);
    RUN_TEST(test_string_writes_are_space_padded);
    RUN_TEST(test_invalid_string_writes_rejected_before_io);
    RUN_TEST(test_mixed_batch_uses_one_bracket_and_one_erase);
    RUN_TEST(test_batch_rejects_bad_items_without_io);
    RUN_TEST(test_batch_flash_patch_limit);
    RUN_TEST(test_batch_keeps_voltage_link_checks);
    RUN_TEST(test_flash_verification_mismatch_keeps_goodset_invalid);
    RUN_TEST(test_invalid_goodset_blocks_writes_before_cal);
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
