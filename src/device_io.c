/*
 * device_io.c — Memory reads and writes and FLASH functions over the transport
 */

#include "internal.h"

/* ------------------------------------------------------------------ */
/* Memory read/write                                                   */
/* ------------------------------------------------------------------ */

static alltrax_error exchange(alltrax_controller* ctrl,
    const uint8_t request[PACKET_SIZE], uint8_t response[PACKET_SIZE])
{
    alltrax_error rc = transport_write(ctrl, request);
    if (rc) return rc;
    rc = transport_read(ctrl, response);
    if (rc != ALLTRAX_ERR_TIMEOUT) return rc;

    rc = transport_drain(ctrl);
    if (rc) return rc;
    rc = transport_write(ctrl, request);
    if (!rc)
        rc = transport_read(ctrl, response);

    /* A late reply to the first send may have satisfied the second read.
     * Discard any duplicate before issuing a different request. */
    alltrax_error drain_rc = transport_drain(ctrl);
    return drain_rc ? drain_rc : rc;
}

alltrax_error read_memory(alltrax_controller* ctrl, uint32_t addr,
    size_t num_bytes, uint8_t* buf)
{
    if (num_bytes < 1 || num_bytes > MAX_PAYLOAD) {
        set_error_detail(ctrl, "num_bytes must be 1-%d, got %zu",
                         MAX_PAYLOAD, num_bytes);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    uint8_t request[PACKET_SIZE];
    build_read_request(addr, (uint8_t)num_bytes, request);

    uint8_t response[PACKET_SIZE];
    alltrax_error rc = exchange(ctrl, request, response);
    if (rc) return rc;

    uint8_t result, n;
    uint8_t payload[MAX_PAYLOAD];
    rc = parse_response(response, RESPONSE_TYPE_RW, &result, &n, payload);
    if (rc) {
        set_error_detail(ctrl,
            "Malformed response reading 0x%08X: type=0x%02X%s, "
            "function=0x%02X, expected type=0x%02X",
            addr, response[4],
            response[4] == RESPONSE_TYPE_UNK ? " (unknown semantics)" : "",
            response[5], RESPONSE_TYPE_RW);
        return rc;
    }

    if (result != RESULT_PASS) {
        set_error_detail(ctrl, "Read failed at 0x%08X: result=0x%02X",
                         addr, result);
        return ALLTRAX_ERR_DEVICE_FAIL;
    }

    /* Callers size their buffers for the request, not the reply's claim. */
    if (n != num_bytes) {
        set_error_detail(ctrl,
            "Read length mismatch at 0x%08X: expected %zu bytes, got %u",
            addr, num_bytes, (unsigned)n);
        return ALLTRAX_ERR_PROTOCOL;
    }

    memcpy(buf, payload, num_bytes);
    return ALLTRAX_OK;
}

alltrax_error read_memory_range(alltrax_controller* ctrl, uint32_t addr,
    size_t total_bytes, uint8_t* buf)
{
    size_t offset = 0;
    while (offset < total_bytes) {
        size_t chunk = total_bytes - offset;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;

        alltrax_error rc = read_memory(ctrl, addr + (uint32_t)offset,
                                                chunk, buf + offset);
        if (rc) return rc;
        offset += chunk;
    }
    return ALLTRAX_OK;
}

/* Write len bytes and require the echo of the first echo_len to match. */
alltrax_error write_memory_echo(alltrax_controller* ctrl, uint32_t addr,
    const uint8_t* data, size_t len, size_t echo_len)
{
    if (len < 1 || len > MAX_PAYLOAD) {
        set_error_detail(ctrl, "Write length must be 1-%d, got %zu",
                         MAX_PAYLOAD, len);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    uint8_t request[PACKET_SIZE];
    build_write_request(addr, data, (uint8_t)len, request);

    uint8_t response[PACKET_SIZE];
    alltrax_error rc = exchange(ctrl, request, response);
    if (rc) return rc;

    uint8_t result, n;
    uint8_t echo[MAX_PAYLOAD];
    rc = parse_response(response, RESPONSE_TYPE_RW, &result, &n, echo);
    if (rc) {
        set_error_detail(ctrl,
            "Malformed write response at 0x%08X: type=0x%02X%s, "
            "function=0x%02X, expected type=0x%02X",
            addr, response[4],
            response[4] == RESPONSE_TYPE_UNK ? " (unknown semantics)" : "",
            response[5], RESPONSE_TYPE_RW);
        return rc;
    }

    if (result != RESULT_PASS) {
        set_error_detail(ctrl, "Write failed at 0x%08X: result=0x%02X",
                         addr, result);
        return ALLTRAX_ERR_DEVICE_FAIL;
    }

    if (n != (uint8_t)len || memcmp(echo, data, echo_len) != 0) {
        set_error_detail(ctrl, "Write echo mismatch at 0x%08X", addr);
        return ALLTRAX_ERR_VERIFY;
    }

    return ALLTRAX_OK;
}

alltrax_error write_memory(alltrax_controller* ctrl, uint32_t addr,
    const uint8_t* data, size_t len)
{
    return write_memory_echo(ctrl, addr, data, len, len);
}

/* ------------------------------------------------------------------ */
/* FLASH functions                                                     */
/* ------------------------------------------------------------------ */

static alltrax_error flash_command(alltrax_controller* ctrl,
    const uint8_t request[PACKET_SIZE], uint8_t* n, uint8_t payload[MAX_PAYLOAD])
{
    uint8_t response[PACKET_SIZE];
    alltrax_error rc = exchange(ctrl, request, response);
    if (rc) return rc;

    uint8_t result;
    rc = parse_response(response, RESPONSE_TYPE_SP, &result, n, payload);
    if (rc || response[5] != request[7]) {
        set_error_detail(ctrl,
            "Malformed FLASH function %u response: type=0x%02X, function=0x%02X",
            request[7], response[4], response[5]);
        return ALLTRAX_ERR_PROTOCOL;
    }

    if (result != RESULT_PASS) {
        set_error_detail(ctrl, "FLASH function %u failed: result=0x%02X",
                         request[7], result);
        return ALLTRAX_ERR_DEVICE_FAIL;
    }

    return ALLTRAX_OK;
}

static alltrax_error clear_flash_errors(alltrax_controller* ctrl)
{
    uint8_t request[PACKET_SIZE], payload[MAX_PAYLOAD], n;
    build_flash_get_flag_request(FLASH_FLAG_WRPRTERR, request);
    alltrax_error rc = flash_command(ctrl, request, &n, payload);
    if (rc) return rc;
    if (n < 4 || payload[3] > 1) {
        set_error_detail(ctrl, "Malformed FLASH write-protection status");
        return ALLTRAX_ERR_PROTOCOL;
    }
    bool protected = payload[3] != 0;

    build_flash_clear_flags_request(
        FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR, request);
    rc = flash_command(ctrl, request, &n, payload);
    if (rc) return rc;
    if (protected) {
        set_error_detail(ctrl, "FLASH write protection is enabled (WRPRTERR)");
        return ALLTRAX_ERR_FLASH;
    }
    return ALLTRAX_OK;
}

static bool flash_failure_can_retry(alltrax_error rc)
{
    return rc == ALLTRAX_ERR_DEVICE_FAIL || rc == ALLTRAX_ERR_TIMEOUT;
}

alltrax_error page_erase(alltrax_controller* ctrl)
{
    uint8_t request[PACKET_SIZE], payload[MAX_PAYLOAD], n;
    build_page_erase_request(VARSET_PAGE, request);
    alltrax_error rc = flash_command(ctrl, request, &n, payload);
    if (flash_failure_can_retry(rc)) {
        rc = clear_flash_errors(ctrl);
        if (!rc)
            rc = flash_command(ctrl, request, &n, payload);
    }
    return rc == ALLTRAX_ERR_DEVICE_FAIL ? ALLTRAX_ERR_FLASH : rc;
}

alltrax_error program_flash(alltrax_controller* ctrl, uint32_t addr,
    const uint8_t* data, size_t len)
{
    alltrax_error rc = write_memory(ctrl, addr, data, len);
    if (flash_failure_can_retry(rc)) {
        rc = clear_flash_errors(ctrl);
        if (!rc)
            rc = write_memory(ctrl, addr, data, len);
    }
    return rc;
}
