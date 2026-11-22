/*
 * Controller regression tests with a simulated transport.
 * This executable does not link transport.c or hidapi and cannot access USB.
 */

#include "helpers.h"
#include "internal.h"

int test_pass_count;
int test_fail_count;

static struct {
    alltrax_controller ctrl;
    uint8_t page[VARSET_SIZE];
    uint8_t request[PACKET_SIZE];
    uint8_t mode;
    unsigned erases, settings_writes, goodset_clears;
    unsigned requests;
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
    if (addr >= ADDR_CONTROLLER_INFO && addr < ADDR_CONTROLLER_INFO + sizeof(model))
        return (uint8_t)model[addr - ADDR_CONTROLLER_INFO];
    if (addr >= ADDR_BOOT_REV && addr < ADDR_BOOT_REV + sizeof(boot_rev))
        return boot_rev[addr - ADDR_BOOT_REV];
    if (addr >= ADDR_PRGM_REV && addr < ADDR_PRGM_REV + sizeof(program_rev))
        return program_rev[addr - ADDR_PRGM_REV];
    if (addr == ADDR_RUN_MODE)
        return mock.mode;
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

    if (data[0] == 0x01)
        return ALLTRAX_OK;
    if (data[0] == 0x02) {
        if (addr == ADDR_RUN_MODE && data[1] == 1) {
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
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
