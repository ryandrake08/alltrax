/*
 * test_protocol.c — Byte-for-byte validation of protocol
 *
 * Ground truth: captures/port2.pcapng, captures/port2-test2.pcapng
 */

#include "helpers.h"
#include "alltrax.h"
#include "internal.h"

/* ------------------------------------------------------------------ */
/* Helper: parse hex string to byte array                              */
/* ------------------------------------------------------------------ */

static int hex_to_bytes(const char* hex, uint8_t* out, size_t max_len)
{
    size_t hex_len = strlen(hex);
    size_t byte_len = hex_len / 2;
    if (byte_len > max_len) return -1;

    for (size_t i = 0; i < byte_len; i++) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%2x", &val) != 1) return -1;
        out[i] = (uint8_t)val;
    }
    return (int)byte_len;
}

/* ------------------------------------------------------------------ */
/* 1. Read request construction — byte-identical to Wireshark captures */
/* ------------------------------------------------------------------ */

/* Captured OUT packets from port2.pcapng */
struct read_request_test {
    uint32_t address;
    uint8_t  num_bytes;
    const char* expected_hex;
};

static const struct read_request_test read_request_tests[] = {
    { 0x08000800, 0x34, "01340000080008000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08001800, 0x02, "01020000080018000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x080067F0, 0x04, "01040000080067f00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x0801FFF0, 0x06, "010600000801fff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08000880, 0x18, "01180000080008800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08001000, 0x02, "01020000080010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002000, 0x02, "01020000080020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002002, 0x02, "01020000080020020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002020, 0x08, "01080000080020200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002040, 0x38, "01380000080020400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002078, 0x2C, "012c0000080020780000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x080020BA, 0x06, "01060000080020ba0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002200, 0x20, "01200000080022000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x080010A0, 0x01, "01010000080010a00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x080020A0, 0x01, "01010000080020a00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x080020A0, 0x05, "01050000080020a00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x080020C0, 0x10, "01100000080020c00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002100, 0x38, "01380000080021000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002138, 0x08, "01080000080021380000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002140, 0x38, "01380000080021400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x08002178, 0x38, "01380000080021780000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    { 0x080021B0, 0x10, "01100000080021b00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
};

static int test_read_request_matches_capture(void)
{
    size_t n = sizeof(read_request_tests) / sizeof(read_request_tests[0]);
    for (size_t i = 0; i < n; i++) {
        const struct read_request_test* t = &read_request_tests[i];
        uint8_t buf[PACKET_SIZE];
        uint8_t expected[PACKET_SIZE];

        build_read_request(t->address, t->num_bytes, buf);
        hex_to_bytes(t->expected_hex, expected, PACKET_SIZE);

        ASSERT_MEM_EQ(buf, expected, PACKET_SIZE);
    }
    return 0;
}

static int test_read_request_is_64_bytes(void)
{
    uint8_t buf[PACKET_SIZE];
    build_read_request(0x08000800, 52, buf);
    /* Verifying construction fills 64 bytes (memset to 0 + header) */
    ASSERT_EQ(buf[0], 0x01);
    ASSERT_EQ(buf[1], 52);
    return 0;
}

static int test_read_request_routing_is_zero(void)
{
    uint8_t buf[PACKET_SIZE];
    build_read_request(0x08000800, 52, buf);
    ASSERT_EQ(buf[2], 0x00);
    ASSERT_EQ(buf[3], 0x00);
    return 0;
}

static int test_read_request_address_big_endian(void)
{
    uint8_t buf[PACKET_SIZE];
    build_read_request(0x08000800, 52, buf);
    ASSERT_EQ(buf[4], 0x08);
    ASSERT_EQ(buf[5], 0x00);
    ASSERT_EQ(buf[6], 0x08);
    ASSERT_EQ(buf[7], 0x00);
    return 0;
}

static int test_read_request_data_bytes_zero(void)
{
    uint8_t buf[PACKET_SIZE];
    uint8_t zeros[MAX_PAYLOAD];
    memset(zeros, 0, sizeof(zeros));
    build_read_request(0x08000800, 52, buf);
    ASSERT_MEM_EQ(buf + 8, zeros, MAX_PAYLOAD);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 2. Response parsing — from captured responses                       */
/* ------------------------------------------------------------------ */

static int test_parse_controller_info(void)
{
    /* Response for 52-byte read from ADDR_CONTROLLER_INFO (0x08000800) */
    const char* hex = "043400000100000058435434383430302d44435320202000392f352f3230323520202020202020000c9f03008a1300008d1300000b0000000400000000000000";
    uint8_t response[PACKET_SIZE];
    hex_to_bytes(hex, response, PACKET_SIZE);

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    alltrax_error err = parse_response(response, RESPONSE_TYPE_RW, &result, &num_bytes, payload);
    ASSERT_EQ(err, ALLTRAX_OK);
    ASSERT_EQ(result, 0x00);
    ASSERT_EQ(num_bytes, 0x34);

    /* Model: 15 chars at offset 0, space-padded */
    char model[16] = {0};
    memcpy(model, payload, 15);
    for (int i = 14; i >= 0 && (model[i] == ' ' || model[i] == '\0'); i--)
        model[i] = '\0';
    ASSERT_STR_EQ(model, "XCT48400-DCS");

    /* Build date: 15 chars at offset 0x10 */
    char build_date[16] = {0};
    memcpy(build_date, payload + 0x10, 15);
    for (int i = 14; i >= 0 && (build_date[i] == ' ' || build_date[i] == '\0'); i--)
        build_date[i] = '\0';
    ASSERT_STR_EQ(build_date, "9/5/2025");

    ASSERT_EQ(get_le32(payload + 0x20), 237324u);   /* serial_number */
    ASSERT_EQ(get_le32(payload + 0x24), 5002u);      /* original_boot_rev */
    ASSERT_EQ(get_le32(payload + 0x28), 5005u);      /* original_program_rev */
    ASSERT_EQ(get_le32(payload + 0x2C), 11u);         /* program_type */
    ASSERT_EQ(get_le32(payload + 0x30), 4u);           /* hardware_rev */
    return 0;
}

static int test_parse_boot_rev(void)
{
    const char* hex = "04040000010000008a13000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
    uint8_t response[PACKET_SIZE];
    hex_to_bytes(hex, response, PACKET_SIZE);

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    parse_response(response, RESPONSE_TYPE_RW, &result, &num_bytes, payload);
    ASSERT_EQ(result, 0x00);

    uint32_t boot_rev = (uint32_t)payload[0]
                      | ((uint32_t)payload[1] << 8)
                      | ((uint32_t)payload[2] << 16)
                      | ((uint32_t)payload[3] << 24);
    ASSERT_EQ(boot_rev, 5002);
    return 0;
}

static int test_parse_prgm_rev(void)
{
    const char* hex = "04060000010000008d13000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
    uint8_t response[PACKET_SIZE];
    hex_to_bytes(hex, response, PACKET_SIZE);

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    parse_response(response, RESPONSE_TYPE_RW, &result, &num_bytes, payload);
    ASSERT_EQ(result, 0x00);

    uint32_t prgm_rev = (uint32_t)payload[0]
                      | ((uint32_t)payload[1] << 8)
                      | ((uint32_t)payload[2] << 16)
                      | ((uint32_t)payload[3] << 24);
    uint16_t prgm_ver = (uint16_t)payload[4]
                      | ((uint16_t)payload[5] << 8);
    ASSERT_EQ(prgm_rev, 5005);
    ASSERT_EQ(prgm_ver, 0);
    return 0;
}

static int test_parse_hardware_config(void)
{
    /* Response for 24-byte read from ADDR_HARDWARE_CONFIG (0x08000880) */
    const char* hex = "041800000100000030009001320000000100010000000000ff070000000100000000000000000000000000000000000000000000000000000000000000000000";
    uint8_t response[PACKET_SIZE];
    hex_to_bytes(hex, response, PACKET_SIZE);

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    parse_response(response, RESPONSE_TYPE_RW, &result, &num_bytes, payload);
    ASSERT_EQ(result, 0x00);
    ASSERT_EQ(num_bytes, 24);

    ASSERT_EQ(get_le16(payload),      48);    /* rated_voltage */
    ASSERT_EQ(get_le16(payload + 2),  400);   /* rated_amps */
    ASSERT_EQ(get_le16(payload + 4),  50);    /* rated_field_amps */
    ASSERT_EQ(payload[6],             0);     /* speed_sensor */
    ASSERT_EQ(payload[8],             1);     /* has_bms_can */
    ASSERT_EQ(payload[9],             0);     /* has_throt_can */
    ASSERT_EQ(payload[10],            1);     /* has_user2 */
    ASSERT_EQ(payload[11],            0);     /* has_user3 */
    ASSERT_EQ(payload[12],            0);     /* has_aux_out1 */
    ASSERT_EQ(payload[13],            0);     /* has_aux_out2 */
    ASSERT_EQ(get_le32(payload + 16), 0x07FFu); /* throttles_allowed */
    ASSERT_EQ(payload[20],            0);     /* has_forward */
    ASSERT_EQ(payload[21],            1);     /* has_user1 */
    ASSERT_EQ(payload[22],            0);     /* can_high_side */
    ASSERT_EQ(payload[23],            0);     /* is_stock_mode */
    return 0;
}

static int test_parse_program_name(void)
{
    const char* hex = "0420000001000000455a5044532d4443532d56312020202020202020202020202020202020202020000000000000000000000000000000000000000000000000";
    uint8_t response[PACKET_SIZE];
    hex_to_bytes(hex, response, PACKET_SIZE);

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    parse_response(response, RESPONSE_TYPE_RW, &result, &num_bytes, payload);

    /* Trim trailing spaces/nulls and check */
    char name[33];
    memcpy(name, payload, 32);
    name[32] = '\0';
    /* Trim */
    int end = 31;
    while (end >= 0 && (name[end] == ' ' || name[end] == '\0'))
        name[end--] = '\0';

    ASSERT_STR_EQ(name, "EZPDS-DCS-V1");
    return 0;
}

static int test_parse_response_rejects_wrong_id(void)
{
    uint8_t bad[PACKET_SIZE];
    memset(bad, 0, sizeof(bad));
    bad[0] = 0x01;  /* Should be 0x04 */

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    alltrax_error err = parse_response(bad, RESPONSE_TYPE_RW, &result, &num_bytes, payload);
    ASSERT_EQ(err, ALLTRAX_ERR_PROTOCOL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 3. Write request construction — from port2-test2.pcapng             */
/* ------------------------------------------------------------------ */

struct write_request_test {
    uint32_t    address;
    uint8_t     data;
    const char* expected_hex;
};

static const struct write_request_test write_request_tests[] = {
    /* CAL mode: write 0xFF to RunMode */
    { 0x2000FFFA, 0xFF,
      "020100002000fffaff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    /* Fan_On: write 0x01 */
    { 0x2000F204, 0x01,
      "020100002000f2040100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
    /* RUN mode: write 0x00 to RunMode */
    { 0x2000FFFA, 0x00,
      "020100002000fffa0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" },
};

static int test_write_request_matches_capture(void)
{
    size_t n = sizeof(write_request_tests) / sizeof(write_request_tests[0]);
    for (size_t i = 0; i < n; i++) {
        const struct write_request_test* t = &write_request_tests[i];
        uint8_t buf[PACKET_SIZE];
        uint8_t expected[PACKET_SIZE];

        build_write_request(t->address, &t->data, 1, buf);
        hex_to_bytes(t->expected_hex, expected, PACKET_SIZE);

        ASSERT_MEM_EQ(buf, expected, PACKET_SIZE);
    }
    return 0;
}

static int test_write_request_is_64_bytes(void)
{
    uint8_t buf[PACKET_SIZE];
    uint8_t data = 0xFF;
    build_write_request(0x2000FFFA, &data, 1, buf);
    ASSERT_EQ(buf[0], 0x02);
    return 0;
}

static int test_write_request_data_at_byte_8(void)
{
    uint8_t buf[PACKET_SIZE];
    uint8_t data = 0x01;
    build_write_request(0x2000F204, &data, 1, buf);
    ASSERT_EQ(buf[8], 0x01);
    /* Rest should be zero */
    for (int i = 9; i < PACKET_SIZE; i++)
        ASSERT_EQ(buf[i], 0x00);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 4. Write response parsing                                           */
/* ------------------------------------------------------------------ */

static void make_write_response(uint8_t* buf, uint8_t num_bytes,
    uint8_t echo_byte, uint8_t result_code)
{
    memset(buf, 0, PACKET_SIZE);
    buf[0] = RESPONSE_ID;
    buf[1] = num_bytes;
    buf[4] = 0x01;  /* write response type */
    buf[6] = result_code;
    buf[8] = echo_byte;
}

static int test_parse_write_response_success(void)
{
    uint8_t response[PACKET_SIZE];
    make_write_response(response, 1, 0xFF, 0x00);

    uint8_t result, num_bytes;
    uint8_t echo[MAX_PAYLOAD];
    alltrax_error err = parse_response(response, RESPONSE_TYPE_RW, &result, &num_bytes, echo);
    ASSERT_EQ(err, ALLTRAX_OK);
    ASSERT_EQ(result, 0x00);
    ASSERT_EQ(num_bytes, 1);
    ASSERT_EQ(echo[0], 0xFF);
    return 0;
}

static int test_parse_write_response_rejects_wrong_id(void)
{
    uint8_t buf[PACKET_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01;

    uint8_t result, num_bytes;
    uint8_t echo[MAX_PAYLOAD];
    alltrax_error err = parse_response(buf, RESPONSE_TYPE_RW, &result, &num_bytes, echo);
    ASSERT_EQ(err, ALLTRAX_ERR_PROTOCOL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 5. Report ID 3 — Special function packets                           */
/* ------------------------------------------------------------------ */

static int test_page_erase_request_format(void)
{
    uint8_t buf[PACKET_SIZE];
    build_page_erase_request(4, buf);

    ASSERT_EQ(buf[0], REPORT_ID_SPECIAL);
    ASSERT_EQ(buf[1], 4);
    ASSERT_EQ(buf[2], 0x00);
    ASSERT_EQ(buf[3], 0x00);
    /* Function code 1 in big-endian */
    ASSERT_EQ(buf[4], 0x00);
    ASSERT_EQ(buf[5], 0x00);
    ASSERT_EQ(buf[6], 0x00);
    ASSERT_EQ(buf[7], 0x01);
    /* Page number in data[3] */
    ASSERT_EQ(buf[8], 0x00);
    ASSERT_EQ(buf[9], 0x00);
    ASSERT_EQ(buf[10], 0x00);
    ASSERT_EQ(buf[11], 0x04);
    /* Rest is zero */
    for (int i = 12; i < PACKET_SIZE; i++)
        ASSERT_EQ(buf[i], 0x00);
    return 0;
}

static int test_page_erase_different_pages(void)
{
    uint8_t pages[] = { 0, 1, 4, 63 };
    for (size_t i = 0; i < sizeof(pages); i++) {
        uint8_t buf[PACKET_SIZE];
        build_page_erase_request(pages[i], buf);
        ASSERT_EQ(buf[11], pages[i]);
    }
    return 0;
}

static int test_reset_request_format(void)
{
    uint8_t buf[PACKET_SIZE];
    build_reset_request(buf);

    ASSERT_EQ(buf[0], REPORT_ID_SPECIAL);
    ASSERT_EQ(buf[1], 0);
    ASSERT_EQ(buf[2], 0x00);
    ASSERT_EQ(buf[3], 0x00);
    /* Function code 12 (0x0C) in big-endian */
    ASSERT_EQ(buf[4], 0x00);
    ASSERT_EQ(buf[5], 0x00);
    ASSERT_EQ(buf[6], 0x00);
    ASSERT_EQ(buf[7], 0x0C);
    /* No data */
    for (int i = 8; i < PACKET_SIZE; i++)
        ASSERT_EQ(buf[i], 0x00);
    return 0;
}

static int test_flash_get_flag_request(void)
{
    uint8_t buf[PACKET_SIZE];
    build_flash_get_flag_request(FLASH_FLAG_WRPRTERR, buf);

    ASSERT_EQ(buf[0], REPORT_ID_SPECIAL);
    ASSERT_EQ(buf[1], 4);
    /* Function 10 (0x0A) */
    ASSERT_EQ(buf[4], 0x00);
    ASSERT_EQ(buf[5], 0x00);
    ASSERT_EQ(buf[6], 0x00);
    ASSERT_EQ(buf[7], 0x0A);
    /* Flag in data[3] */
    ASSERT_EQ(buf[11], 0x10);
    return 0;
}

static int test_flash_clear_flags_all(void)
{
    uint8_t flags = FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR;
    uint8_t buf[PACKET_SIZE];
    build_flash_clear_flags_request(flags, buf);

    ASSERT_EQ(buf[0], REPORT_ID_SPECIAL);
    ASSERT_EQ(buf[1], 4);
    /* Function 11 (0x0B) */
    ASSERT_EQ(buf[7], 0x0B);
    /* EOP(32) + PGERR(4) + WRPRTERR(16) = 52 = 0x34 */
    ASSERT_EQ(buf[11], 0x34);
    return 0;
}

static int test_flash_clear_flags_selective(void)
{
    uint8_t buf[PACKET_SIZE];
    build_flash_clear_flags_request(FLASH_FLAG_WRPRTERR, buf);
    ASSERT_EQ(buf[11], 0x10);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 6. Special function response parsing                                */
/* ------------------------------------------------------------------ */

static int test_parse_special_response_success(void)
{
    uint8_t response[PACKET_SIZE];
    memset(response, 0, sizeof(response));
    response[0] = RESPONSE_ID;
    response[1] = 0;
    response[4] = RESPONSE_TYPE_SP;
    response[6] = 0x00;

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    alltrax_error err = parse_response(response, RESPONSE_TYPE_SP, &result, &num_bytes, payload);
    ASSERT_EQ(err, ALLTRAX_OK);
    ASSERT_EQ(result, 0x00);
    ASSERT_EQ(num_bytes, 0);
    return 0;
}

static int test_parse_special_response_with_data(void)
{
    uint8_t response[PACKET_SIZE];
    memset(response, 0, sizeof(response));
    response[0] = RESPONSE_ID;
    response[1] = 4;
    response[4] = RESPONSE_TYPE_SP;
    response[6] = 0x00;
    response[8] = 0x00;
    response[9] = 0x00;
    response[10] = 0x00;
    response[11] = 0x01;  /* flag is set */

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    parse_response(response, RESPONSE_TYPE_SP, &result, &num_bytes, payload);
    ASSERT_EQ(result, 0x00);
    ASSERT_EQ(num_bytes, 4);
    ASSERT_EQ(payload[3], 0x01);
    return 0;
}

static int test_parse_special_response_failure(void)
{
    uint8_t response[PACKET_SIZE];
    memset(response, 0, sizeof(response));
    response[0] = RESPONSE_ID;
    response[4] = RESPONSE_TYPE_SP;
    response[6] = 0x01;

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    parse_response(response, RESPONSE_TYPE_SP, &result, &num_bytes, payload);
    ASSERT_EQ(result, 0x01);
    return 0;
}

static int test_parse_special_response_rejects_wrong_id(void)
{
    uint8_t bad[PACKET_SIZE];
    memset(bad, 0, sizeof(bad));
    bad[0] = 0x01;

    uint8_t result, num_bytes;
    uint8_t payload[MAX_PAYLOAD];
    alltrax_error err = parse_response(bad, RESPONSE_TYPE_SP, &result, &num_bytes, payload);
    ASSERT_EQ(err, ALLTRAX_ERR_PROTOCOL);
    return 0;
}


/* ------------------------------------------------------------------ */
/* Test runner                                                         */
/* ------------------------------------------------------------------ */

void run_protocol_tests(void)
{
    printf("protocol:\n");

    /* Read request construction */
    RUN_TEST(test_read_request_matches_capture);
    RUN_TEST(test_read_request_is_64_bytes);
    RUN_TEST(test_read_request_routing_is_zero);
    RUN_TEST(test_read_request_address_big_endian);
    RUN_TEST(test_read_request_data_bytes_zero);

    /* Response parsing */
    RUN_TEST(test_parse_controller_info);
    RUN_TEST(test_parse_boot_rev);
    RUN_TEST(test_parse_prgm_rev);
    RUN_TEST(test_parse_hardware_config);
    RUN_TEST(test_parse_program_name);
    RUN_TEST(test_parse_response_rejects_wrong_id);

    /* Write request construction */
    RUN_TEST(test_write_request_matches_capture);
    RUN_TEST(test_write_request_is_64_bytes);
    RUN_TEST(test_write_request_data_at_byte_8);

    /* Write response parsing */
    RUN_TEST(test_parse_write_response_success);
    RUN_TEST(test_parse_write_response_rejects_wrong_id);

    /* Special function packets */
    RUN_TEST(test_page_erase_request_format);
    RUN_TEST(test_page_erase_different_pages);
    RUN_TEST(test_reset_request_format);
    RUN_TEST(test_flash_get_flag_request);
    RUN_TEST(test_flash_clear_flags_all);
    RUN_TEST(test_flash_clear_flags_selective);

    /* Special response parsing */
    RUN_TEST(test_parse_special_response_success);
    RUN_TEST(test_parse_special_response_with_data);
    RUN_TEST(test_parse_special_response_failure);
    RUN_TEST(test_parse_special_response_rejects_wrong_id);

}
