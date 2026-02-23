/*
 * protocol.c — Packet construction and parsing (pure functions)
 *
 * Packet format (64 bytes):
 *   Request:  [ReportID][numBytes][route_lo][route_hi][addr_BE_4][data...]
 *   Response: [0x04][numBytes][0x00][0x00][type][0x00][result][0x00][data...]
 *
 * Addresses are big-endian in packets; data payload is little-endian.
 */

#include "internal.h"

/* Store a uint32 in big-endian at buf */
static void put_be32(uint8_t* buf, uint32_t val)
{
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

/* Read a uint16 in little-endian from buf */
static uint16_t get_le16(const uint8_t* buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/* Read a uint32 in little-endian from buf */
static uint32_t get_le32(const uint8_t* buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

void build_read_request(uint32_t addr, uint8_t num_bytes, uint8_t buf[PACKET_SIZE])
{
    memset(buf, 0, PACKET_SIZE);
    buf[0] = REPORT_ID_READ;
    buf[1] = num_bytes;
    /* bytes 2-3: routing = 0x00 (XCT, no checksum) */
    put_be32(buf + 4, addr);
}

alltrax_error parse_read_response(const uint8_t data[PACKET_SIZE],
    uint8_t* result, uint8_t* num_bytes, uint8_t payload[MAX_PAYLOAD])
{
    if (data[0] != RESPONSE_ID)
        return ALLTRAX_ERR_PROTOCOL;

    *num_bytes = data[1];
    /* data[4] = 0x00 for read responses */
    *result = data[6];

    if (*num_bytes > MAX_PAYLOAD)
        *num_bytes = MAX_PAYLOAD;

    memcpy(payload, data + 8, *num_bytes);
    return ALLTRAX_OK;
}

void build_write_request(uint32_t addr, const uint8_t* data, uint8_t data_len,
    uint8_t buf[PACKET_SIZE])
{
    memset(buf, 0, PACKET_SIZE);
    buf[0] = REPORT_ID_WRITE;
    buf[1] = data_len;
    put_be32(buf + 4, addr);
    memcpy(buf + 8, data, data_len);
}

alltrax_error parse_write_response(const uint8_t data[PACKET_SIZE],
    uint8_t* result, uint8_t* num_bytes, uint8_t echo[MAX_PAYLOAD])
{
    if (data[0] != RESPONSE_ID)
        return ALLTRAX_ERR_PROTOCOL;

    if (data[4] != 0x01)
        return ALLTRAX_ERR_PROTOCOL;

    *num_bytes = data[1];
    *result = data[6];

    if (*num_bytes > MAX_PAYLOAD)
        *num_bytes = MAX_PAYLOAD;

    memcpy(echo, data + 8, *num_bytes);
    return ALLTRAX_OK;
}

void build_page_erase_request(uint8_t page, uint8_t buf[PACKET_SIZE])
{
    memset(buf, 0, PACKET_SIZE);
    buf[0] = REPORT_ID_SPECIAL;
    buf[1] = 4;  /* numBytes = 4 (data length) */
    put_be32(buf + 4, FUNC_PAGE_ERASE);
    /* Data: [0x00, 0x00, 0x00, page_number] */
    buf[11] = page;
}

void build_reset_request(uint8_t buf[PACKET_SIZE])
{
    memset(buf, 0, PACKET_SIZE);
    buf[0] = REPORT_ID_SPECIAL;
    buf[1] = 0;  /* numBytes = 0 (no data) */
    put_be32(buf + 4, FUNC_RESET);
}

void build_flash_get_flag_request(uint8_t flag, uint8_t buf[PACKET_SIZE])
{
    memset(buf, 0, PACKET_SIZE);
    buf[0] = REPORT_ID_SPECIAL;
    buf[1] = 4;
    put_be32(buf + 4, FUNC_GET_FLAG_STATUS);
    buf[11] = flag;
}

void build_flash_clear_flags_request(uint8_t flags, uint8_t buf[PACKET_SIZE])
{
    memset(buf, 0, PACKET_SIZE);
    buf[0] = REPORT_ID_SPECIAL;
    buf[1] = 4;
    put_be32(buf + 4, FUNC_CLEAR_FLAGS);
    buf[11] = flags;
}

alltrax_error parse_special_response(const uint8_t data[PACKET_SIZE],
    uint8_t* result, uint8_t* num_bytes, uint8_t payload[MAX_PAYLOAD])
{
    if (data[0] != RESPONSE_ID)
        return ALLTRAX_ERR_PROTOCOL;

    *num_bytes = data[1];
    *result = data[6];

    if (*num_bytes > MAX_PAYLOAD)
        *num_bytes = MAX_PAYLOAD;

    if (*num_bytes > 0)
        memcpy(payload, data + 8, *num_bytes);

    return ALLTRAX_OK;
}

alltrax_error parse_controller_info(const uint8_t* payload, size_t len,
    alltrax_info* info)
{
    if (len < 52)
        return ALLTRAX_ERR_PROTOCOL;

    /* Model: 15 chars at offset 0, space/null padded */
    memset(info->model, 0, sizeof(info->model));
    memcpy(info->model, payload, 15);
    /* Trim trailing spaces and nulls */
    for (int i = 14; i >= 0; i--) {
        if (info->model[i] == ' ' || info->model[i] == '\0')
            info->model[i] = '\0';
        else
            break;
    }

    /* Build date: 15 chars at offset 0x10 */
    memset(info->build_date, 0, sizeof(info->build_date));
    memcpy(info->build_date, payload + 0x10, 15);
    for (int i = 14; i >= 0; i--) {
        if (info->build_date[i] == ' ' || info->build_date[i] == '\0')
            info->build_date[i] = '\0';
        else
            break;
    }

    info->serial_number       = get_le32(payload + 0x20);
    info->original_boot_rev   = get_le32(payload + 0x24);
    info->original_program_rev = get_le32(payload + 0x28);
    /* program_type at 0x2C — not stored in alltrax_info (available via raw read) */
    /* hardware_rev at 0x30 — not stored in alltrax_info (available via raw read) */

    return ALLTRAX_OK;
}

alltrax_error parse_hardware_config(const uint8_t* payload, size_t len,
    alltrax_info* info)
{
    if (len < 4)
        return ALLTRAX_ERR_PROTOCOL;

    info->rated_voltage = get_le16(payload);
    info->rated_amps    = get_le16(payload + 2);

    return ALLTRAX_OK;
}

char* alltrax_format_rev(uint32_t rev, char* buf, size_t buflen)
{
    uint32_t major = rev / 1000;
    uint32_t minor = rev % 1000;
    snprintf(buf, buflen, "V%u.%03u", major, minor);
    return buf;
}
