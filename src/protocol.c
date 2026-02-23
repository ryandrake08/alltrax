/*
 * protocol.c — Packet construction and parsing (pure functions)
 *
 * Packet format (64 bytes):
 *   Request:  [ReportID][numBytes][route_lo][route_hi][addr_BE_4][data...]
 *   Response: [0x04][numBytes][route_lo][route_hi][type][func][result][0x00][data...]
 *   type: 1=read/write, 2=special.  func: 0 for read/write, function# for special.
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

alltrax_error parse_response(const uint8_t data[PACKET_SIZE],
    uint8_t expected_type, uint8_t* result, uint8_t* num_bytes,
    uint8_t payload[MAX_PAYLOAD])
{
    if (data[0] != RESPONSE_ID)
        return ALLTRAX_ERR_PROTOCOL;

    if (data[1] > MAX_PAYLOAD)
        return ALLTRAX_ERR_PROTOCOL;

    if (data[4] != expected_type)
        return ALLTRAX_ERR_PROTOCOL;

    *num_bytes = data[1];
    *result = data[6];

    if (*num_bytes > 0)
        memcpy(payload, data + 8, *num_bytes);

    return ALLTRAX_OK;
}

void build_read_request(uint32_t addr, uint8_t num_bytes, uint8_t buf[PACKET_SIZE])
{
    memset(buf, 0, PACKET_SIZE);
    buf[0] = REPORT_ID_READ;
    buf[1] = num_bytes;
    /* bytes 2-3: routing = 0x00 (XCT, no checksum) */
    put_be32(buf + 4, addr);
}

void build_write_request(uint32_t addr, const uint8_t* data, uint8_t data_len, uint8_t buf[PACKET_SIZE])
{
    memset(buf, 0, PACKET_SIZE);
    buf[0] = REPORT_ID_WRITE;
    buf[1] = data_len;
    put_be32(buf + 4, addr);
    memcpy(buf + 8, data, data_len);
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
