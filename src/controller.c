/*
 * controller.c — High-level operations: read/write/flash/reset
 */

#include "internal.h"
#include <stdlib.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Memory read/write                                                   */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_read_memory(alltrax_controller* ctrl, uint32_t addr,
    size_t num_bytes, uint8_t* buf, size_t* out_len)
{
    if (num_bytes < 1 || num_bytes > MAX_PAYLOAD) {
        set_error_detail(ctrl, "num_bytes must be 1-%d, got %zu",
                         MAX_PAYLOAD, num_bytes);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    uint8_t request[PACKET_SIZE];
    build_read_request(addr, (uint8_t)num_bytes, request);

    alltrax_error rc = transport_write(ctrl, request);
    if (rc) return rc;

    uint8_t response[PACKET_SIZE];
    rc = transport_read(ctrl, response, 0);
    if (rc) return rc;

    uint8_t result, n;
    uint8_t payload[MAX_PAYLOAD];
    rc = parse_read_response(response, &result, &n, payload);
    if (rc) {
        set_error_detail(ctrl, "Malformed response reading 0x%08X", addr);
        return rc;
    }

    if (result != RESULT_PASS) {
        set_error_detail(ctrl, "Read failed at 0x%08X: result=0x%02X",
                         addr, result);
        return ALLTRAX_ERR_DEVICE_FAIL;
    }

    memcpy(buf, payload, n);
    if (out_len) *out_len = n;
    return ALLTRAX_OK;
}

alltrax_error alltrax_read_memory_range(alltrax_controller* ctrl, uint32_t addr,
    size_t total_bytes, uint8_t* buf)
{
    size_t offset = 0;
    while (offset < total_bytes) {
        size_t chunk = total_bytes - offset;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;

        size_t got;
        alltrax_error rc = alltrax_read_memory(ctrl, addr + (uint32_t)offset,
                                                chunk, buf + offset, &got);
        if (rc) return rc;
        offset += chunk;
    }
    return ALLTRAX_OK;
}

alltrax_error alltrax_write_memory(alltrax_controller* ctrl, uint32_t addr,
    const uint8_t* data, size_t len)
{
    if (len < 1 || len > MAX_PAYLOAD) {
        set_error_detail(ctrl, "Write length must be 1-%d, got %zu",
                         MAX_PAYLOAD, len);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    uint8_t request[PACKET_SIZE];
    build_write_request(addr, data, (uint8_t)len, request);

    alltrax_error rc = transport_write(ctrl, request);
    if (rc) return rc;

    uint8_t response[PACKET_SIZE];
    rc = transport_read(ctrl, response, 0);
    if (rc) return rc;

    uint8_t result, n;
    uint8_t echo[MAX_PAYLOAD];
    rc = parse_write_response(response, &result, &n, echo);
    if (rc) {
        set_error_detail(ctrl, "Malformed write response at 0x%08X", addr);
        return rc;
    }

    if (result != RESULT_PASS) {
        set_error_detail(ctrl, "Write failed at 0x%08X: result=0x%02X",
                         addr, result);
        return ALLTRAX_ERR_DEVICE_FAIL;
    }

    if (n != (uint8_t)len || memcmp(echo, data, len) != 0) {
        set_error_detail(ctrl, "Write echo mismatch at 0x%08X", addr);
        return ALLTRAX_ERR_VERIFY;
    }

    return ALLTRAX_OK;
}

/* Write an arbitrary number of bytes in 56-byte chunks */
static alltrax_error write_memory_range(alltrax_controller* ctrl,
    uint32_t addr, const uint8_t* data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;

        alltrax_error rc = alltrax_write_memory(ctrl, addr + (uint32_t)offset,
                                                 data + offset, chunk);
        if (rc) return rc;
        offset += chunk;
    }
    return ALLTRAX_OK;
}

/* ------------------------------------------------------------------ */
/* Controller info                                                     */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_get_info(alltrax_controller* ctrl, alltrax_info* out)
{
    if (!out)
        return ALLTRAX_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    alltrax_error rc;

    /* Read controller identity block (52 bytes) */
    uint8_t info_buf[56];
    rc = alltrax_read_memory(ctrl, ADDR_CONTROLLER_INFO, 52, info_buf, NULL);
    if (rc) return rc;

    rc = parse_controller_info(info_buf, 52, out);
    if (rc) return rc;

    /* Read hardware config (rated voltage/amps) */
    uint8_t hw_buf[24];
    rc = alltrax_read_memory(ctrl, ADDR_HARDWARE_CONFIG, 24, hw_buf, NULL);
    if (rc) return rc;

    rc = parse_hardware_config(hw_buf, 24, out);
    if (rc) return rc;

    /* Read boot revision */
    uint8_t boot_buf[4];
    rc = alltrax_read_memory(ctrl, ADDR_BOOT_REV, 4, boot_buf, NULL);
    if (rc) return rc;
    out->boot_rev = (uint32_t)boot_buf[0] | ((uint32_t)boot_buf[1] << 8)
                  | ((uint32_t)boot_buf[2] << 16) | ((uint32_t)boot_buf[3] << 24);

    /* Read program revision + version */
    uint8_t prgm_buf[6];
    rc = alltrax_read_memory(ctrl, ADDR_PRGM_REV, 6, prgm_buf, NULL);
    if (rc) return rc;
    out->program_rev = (uint32_t)prgm_buf[0] | ((uint32_t)prgm_buf[1] << 8)
                     | ((uint32_t)prgm_buf[2] << 16) | ((uint32_t)prgm_buf[3] << 24);
    out->program_ver = (uint16_t)prgm_buf[4] | ((uint16_t)prgm_buf[5] << 8);

    /* Cache info */
    ctrl->info = *out;
    ctrl->info_valid = true;

    return ALLTRAX_OK;
}

alltrax_error alltrax_check_firmware(alltrax_controller* ctrl)
{
    if (!ctrl->info_valid) {
        alltrax_info info;
        alltrax_error rc = alltrax_get_info(ctrl, &info);
        if (rc) return rc;
    }

    uint32_t rev = ctrl->info.program_rev;
    if (rev < 1 || rev >= 6000) {
        char buf[16];
        alltrax_format_rev(rev, buf, sizeof(buf));
        set_error_detail(ctrl,
            "Firmware %s (rev %u) is outside known bounds (1-5999). "
            "Reads allowed, writes blocked unless --force",
            buf, rev);
        return ALLTRAX_ERR_FIRMWARE;
    }

    return ALLTRAX_OK;
}

bool alltrax_has_feature(const alltrax_controller* ctrl, alltrax_feature feat)
{
    if (!ctrl->info_valid)
        return false;

    uint32_t orig = ctrl->info.original_program_rev;
    uint32_t prgm = ctrl->info.program_rev;

    switch (feat) {
    case ALLTRAX_FEAT_USER_PROFILES:  return prgm >= 1005;
    case ALLTRAX_FEAT_USER_DEFAULTS:  return orig >= 1007;
    case ALLTRAX_FEAT_CAN_HIGHSIDE:   return orig >= 1008;
    case ALLTRAX_FEAT_FORWARD_INPUT:  return orig >= 68;
    case ALLTRAX_FEAT_USER1_INPUT:    return orig >= 70;
    case ALLTRAX_FEAT_THROTTLE_CAPS:  return orig > 2;
    case ALLTRAX_FEAT_BAD_VARS_CODE:  return prgm >= 1107;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Variable read                                                       */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_read_var(alltrax_controller* ctrl,
    const alltrax_var_def* var, alltrax_var_value* out)
{
    size_t var_size = alltrax_var_byte_size(var);
    if (var_size == 0 || var_size > MAX_PAYLOAD) {
        set_error_detail(ctrl, "Invalid variable size: %zu", var_size);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    uint8_t buf[MAX_PAYLOAD];
    size_t got;
    alltrax_error rc = alltrax_read_memory(ctrl, var->address, var_size,
                                            buf, &got);
    if (rc) return rc;

    return alltrax_decode_var(buf, got, var, var->address, out);
}

alltrax_error alltrax_read_var_group(alltrax_controller* ctrl,
    alltrax_var_group group, alltrax_var_value* out, size_t* count)
{
    size_t n;
    const alltrax_var_def* vars = alltrax_get_var_defs(group, &n);
    if (!vars) {
        *count = 0;
        return ALLTRAX_OK;
    }

    for (size_t i = 0; i < n; i++) {
        alltrax_error rc = alltrax_read_var(ctrl, &vars[i], &out[i]);
        if (rc) {
            *count = i;
            return rc;
        }
    }

    *count = n;
    return ALLTRAX_OK;
}

/* ------------------------------------------------------------------ */
/* RAM variable write (with CAL/RUN bracket)                           */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_write_ram_var(alltrax_controller* ctrl,
    const alltrax_var_def* var, double value)
{
    /* Only RAM addresses allowed */
    if (var->address < RAM_BASE || var->address >= RAM_END) {
        set_error_detail(ctrl,
            "Address 0x%08X is outside RAM range (0x%08X-0x%08X)",
            var->address, RAM_BASE, RAM_END);
        return ALLTRAX_ERR_ADDRESS;
    }

    /* Encode value */
    uint8_t encoded[MAX_PAYLOAD];
    int len = alltrax_encode_var(var, value, encoded);
    if (len <= 0) {
        set_error_detail(ctrl, "Cannot encode variable %s", var->name);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    alltrax_error rc;
    bool in_cal = false;

    /* Read RunMode — confirm in RUN mode */
    uint8_t mode_buf[1];
    rc = alltrax_read_memory(ctrl, ADDR_RUN_MODE, 1, mode_buf, NULL);
    if (rc) return rc;

    if (mode_buf[0] != RUN_MODE_RUN) {
        set_error_detail(ctrl,
            "Controller not in RUN mode (RunMode=0x%02X)", mode_buf[0]);
        return ALLTRAX_ERR_NOT_RUN;
    }

    /* Enter CAL mode */
    uint8_t cal = RUN_MODE_CAL;
    rc = alltrax_write_memory(ctrl, ADDR_RUN_MODE, &cal, 1);
    if (rc) goto cleanup;
    in_cal = true;

    /* Write the actual variable */
    rc = alltrax_write_memory(ctrl, var->address, encoded, (size_t)len);

cleanup:
    if (in_cal) {
        uint8_t run = RUN_MODE_RUN;
        alltrax_error rc2 = alltrax_write_memory(ctrl, ADDR_RUN_MODE, &run, 1);
        if (!rc) rc = rc2;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* FLASH write — full page read-modify-erase-write cycle               */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_page_erase(alltrax_controller* ctrl, uint8_t page)
{
    if (page > 63) {
        set_error_detail(ctrl, "Page must be 0-63, got %d", page);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    uint8_t request[PACKET_SIZE];
    build_page_erase_request(page, request);

    alltrax_error rc = transport_write(ctrl, request);
    if (rc) return rc;

    uint8_t response[PACKET_SIZE];
    rc = transport_read(ctrl, response, 0);
    if (rc) return rc;

    uint8_t result, n;
    uint8_t payload[MAX_PAYLOAD];
    rc = parse_special_response(response, &result, &n, payload);
    if (rc) return rc;

    if (result != RESULT_PASS) {
        set_error_detail(ctrl, "Page erase failed for page %d: result=0x%02X",
                         page, result);
        return ALLTRAX_ERR_FLASH;
    }

    return ALLTRAX_OK;
}

alltrax_error alltrax_reset_device(alltrax_controller* ctrl)
{
    uint8_t request[PACKET_SIZE];
    build_reset_request(request);

    /* Send reset — device disconnects immediately, no response */
    return transport_write(ctrl, request);
}

alltrax_error alltrax_write_flash_var(alltrax_controller* ctrl,
    const alltrax_var_def* var, double value)
{
    return alltrax_write_flash_vars(ctrl, &var, &value, 1);
}

alltrax_error alltrax_write_flash_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count)
{
    if (count == 0) {
        set_error_detail(ctrl, "No variables to write");
        return ALLTRAX_ERR_INVALID_ARG;
    }

    /* 1. Validate all addresses and encode all values (before any USB I/O) */
    typedef struct { size_t offset; uint8_t data[MAX_PAYLOAD]; int len; } patch_t;
    patch_t* patches = calloc(count, sizeof(patch_t));
    if (!patches) return ALLTRAX_ERR_USB;

    for (size_t i = 0; i < count; i++) {
        const alltrax_var_def* var = vars[i];
        if (var->address < VARSET_ADDR ||
            var->address >= VARSET_ADDR + VARSET_SIZE) {
            set_error_detail(ctrl,
                "Variable %s at 0x%08X is not in VARSET page (0x%08X-0x%08X)",
                var->name, var->address, VARSET_ADDR,
                VARSET_ADDR + VARSET_SIZE);
            free(patches);
            return ALLTRAX_ERR_ADDRESS;
        }
        patches[i].offset = var->address - VARSET_ADDR;
        patches[i].len = alltrax_encode_var(var, values[i], patches[i].data);
        if (patches[i].len <= 0) {
            set_error_detail(ctrl, "Cannot encode variable %s", var->name);
            free(patches);
            return ALLTRAX_ERR_INVALID_ARG;
        }
        if (patches[i].offset + (size_t)patches[i].len > VARSET_SIZE) {
            set_error_detail(ctrl, "Variable %s extends beyond VARSET page",
                             var->name);
            free(patches);
            return ALLTRAX_ERR_ADDRESS;
        }
    }

    alltrax_error rc;

    /* 2. Validate firmware version */
    uint8_t prgm_buf[6];
    rc = alltrax_read_memory(ctrl, ADDR_PRGM_REV, 6, prgm_buf, NULL);
    if (rc) { free(patches); return rc; }

    uint32_t prgm_rev = (uint32_t)prgm_buf[0] | ((uint32_t)prgm_buf[1] << 8)
                      | ((uint32_t)prgm_buf[2] << 16) | ((uint32_t)prgm_buf[3] << 24);
    if (prgm_rev != VALIDATED_FIRMWARE) {
        char buf[16];
        alltrax_format_rev(prgm_rev, buf, sizeof(buf));
        set_error_detail(ctrl,
            "Firmware %s is not validated. Only V5.005 is supported",
            buf);
        free(patches);
        return ALLTRAX_ERR_FIRMWARE;
    }

    /* 3. Check GoodSet markers */
    uint8_t gs_buf[2];

    rc = alltrax_read_memory(ctrl, ADDR_V_GOODSET, 2, gs_buf, NULL);
    if (rc) { free(patches); return rc; }
    uint16_t v_gs = (uint16_t)gs_buf[0] | ((uint16_t)gs_buf[1] << 8);

    rc = alltrax_read_memory(ctrl, ADDR_F_GOODSET, 2, gs_buf, NULL);
    if (rc) { free(patches); return rc; }
    uint16_t f_gs = (uint16_t)gs_buf[0] | ((uint16_t)gs_buf[1] << 8);

    if (f_gs != 0x0000) {
        set_error_detail(ctrl,
            "Factory defaults corrupted (F_GoodSet=0x%04X, expected 0x0000)",
            f_gs);
        free(patches);
        return ALLTRAX_ERR_FLASH;
    }
    if (v_gs != 0x0000) {
        set_error_detail(ctrl,
            "User settings invalid (V_GoodSet=0x%04X, expected 0x0000). "
            "Previous write may have been interrupted", v_gs);
        free(patches);
        return ALLTRAX_ERR_FLASH;
    }

    /* 4. Read RunMode — confirm in RUN mode */
    uint8_t mode_buf[1];
    rc = alltrax_read_memory(ctrl, ADDR_RUN_MODE, 1, mode_buf, NULL);
    if (rc) { free(patches); return rc; }

    if (mode_buf[0] != RUN_MODE_RUN) {
        set_error_detail(ctrl,
            "Controller not in RUN mode (RunMode=0x%02X)", mode_buf[0]);
        free(patches);
        return ALLTRAX_ERR_NOT_RUN;
    }

    /* 5-10: CAL bracket with goto cleanup */
    bool in_cal = false;
    uint8_t* page_data = malloc(VARSET_SIZE);
    if (!page_data) { free(patches); return ALLTRAX_ERR_USB; }

    /* Enter CAL mode */
    uint8_t cal = RUN_MODE_CAL;
    rc = alltrax_write_memory(ctrl, ADDR_RUN_MODE, &cal, 1);
    if (rc) goto flash_cleanup;
    in_cal = true;

    /* 6. Read full VARSET page (2048 bytes) */
    rc = alltrax_read_memory_range(ctrl, VARSET_ADDR, VARSET_SIZE, page_data);
    if (rc) goto flash_cleanup;

    /* 7. Patch all variables into page buffer */
    for (size_t i = 0; i < count; i++) {
        memcpy(page_data + patches[i].offset, patches[i].data,
               (size_t)patches[i].len);
    }

    /* Set GoodSet to 0xFFFF (write in progress) */
    page_data[0] = 0xFF;
    page_data[1] = 0xFF;

    /* 8. Erase page */
    rc = alltrax_page_erase(ctrl, VARSET_PAGE);
    if (rc) goto flash_cleanup;

    /* 9. Write full page back */
    rc = write_memory_range(ctrl, VARSET_ADDR, page_data, VARSET_SIZE);
    if (rc) goto flash_cleanup;

    /* 10. Read-back verification */
    {
        uint8_t* readback = malloc(VARSET_SIZE);
        if (!readback) { rc = ALLTRAX_ERR_USB; goto flash_cleanup; }

        rc = alltrax_read_memory_range(ctrl, VARSET_ADDR, VARSET_SIZE, readback);
        if (rc) { free(readback); goto flash_cleanup; }

        if (memcmp(readback, page_data, VARSET_SIZE) != 0) {
            for (size_t i = 0; i < VARSET_SIZE; i++) {
                if (readback[i] != page_data[i]) {
                    set_error_detail(ctrl,
                        "FLASH verification failed at offset 0x%04X: "
                        "wrote 0x%02X, read 0x%02X",
                        (unsigned)i, page_data[i], readback[i]);
                    break;
                }
            }
            free(readback);
            rc = ALLTRAX_ERR_FLASH;
            goto flash_cleanup;
        }
        free(readback);
    }

    /* 11. Clear GoodSet (mark settings as valid) */
    {
        uint8_t zeros[2] = { 0x00, 0x00 };
        rc = alltrax_write_memory(ctrl, VARSET_ADDR, zeros, 2);
    }

flash_cleanup:
    if (in_cal) {
        uint8_t run = RUN_MODE_RUN;
        alltrax_error rc2 = alltrax_write_memory(ctrl, ADDR_RUN_MODE, &run, 1);
        if (!rc) rc = rc2;
    }
    free(page_data);
    free(patches);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Monitoring                                                          */
/* ------------------------------------------------------------------ */

/* Monitor block addresses and sizes */
static const struct { uint32_t addr; size_t size; } monitor_blocks[] = {
    { 0x2000F000, 17 },  /* Error flags */
    { 0x2000F030, 34 },  /* Error history */
    { 0x2000F090, 17 },  /* Digital inputs */
    { 0x2000F0E6,  2 },  /* Temperature */
    { 0x2000F110, 56 },  /* Voltage, current, throttle */
    { 0x2000F148, 22 },  /* Battery, power */
};
#define MONITOR_BLOCK_COUNT 6

alltrax_error alltrax_read_monitor(alltrax_controller* ctrl,
    alltrax_monitor_data* out)
{
    memset(out, 0, sizeof(*out));

    uint8_t blocks[MONITOR_BLOCK_COUNT][MAX_PAYLOAD];
    for (int i = 0; i < MONITOR_BLOCK_COUNT; i++) {
        alltrax_error rc = alltrax_read_memory(ctrl,
            monitor_blocks[i].addr, monitor_blocks[i].size,
            blocks[i], NULL);
        if (rc) return rc;
    }

    /* Block 0: Error flags (17 bytes) */
    memcpy(out->errors, blocks[0], 17);

    /* Block 1: Error history (17 x uint16) */
    for (int i = 0; i < 17; i++) {
        out->error_history[i] = (uint16_t)blocks[1][i * 2]
                              | ((uint16_t)blocks[1][i * 2 + 1] << 8);
    }

    /* Block 2: Digital inputs (17 bytes at 0x2000F090) */
    out->keyswitch  = blocks[2][0] != 0;   /* 0x2000F090 */
    out->reverse    = blocks[2][1] != 0;   /* 0x2000F091 */
    out->relay_on   = (int16_t)((uint16_t)blocks[2][2]
                    | ((uint16_t)blocks[2][3] << 8));  /* 0x2000F092 */
    out->fan_status = blocks[2][4] != 0;   /* 0x2000F094 */
    out->hpd_ran    = blocks[2][5] != 0;   /* 0x2000F095 */
    out->bad_vars_code = (int16_t)((uint16_t)blocks[2][6]
                       | ((uint16_t)blocks[2][7] << 8)); /* 0x2000F096 */
    out->footswitch = blocks[2][8] != 0;   /* 0x2000F098 */
    out->forward    = blocks[2][9] != 0;   /* 0x2000F099 */
    out->user1_input = blocks[2][10] != 0; /* 0x2000F09A */
    out->user2_input = blocks[2][11] != 0; /* 0x2000F09B */
    out->user3_input = blocks[2][12] != 0; /* 0x2000F09C */
    out->charger    = blocks[2][13] != 0;  /* 0x2000F09D */
    out->blower     = blocks[2][14] != 0;  /* 0x2000F09E */
    out->check_motor = blocks[2][15] != 0; /* 0x2000F09F */
    out->horn       = blocks[2][16] != 0;  /* 0x2000F0A0 */

    /* Block 3: Temperature (2 bytes at 0x2000F0E6) */
    {
        int16_t raw = (int16_t)((uint16_t)blocks[3][0]
                    | ((uint16_t)blocks[3][1] << 8));
        out->temp_c = ((double)raw - 527.0) * 0.1289;
    }

    /* Block 4: Power (56 bytes at 0x2000F110) */
    {
        const uint8_t* p = blocks[4];
        int16_t bplus = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        out->battery_volts = (double)bplus * 0.1;

        out->throttle_local = (int16_t)((uint16_t)p[6] | ((uint16_t)p[7] << 8));
        out->throttle_position = (int16_t)((uint16_t)p[8] | ((uint16_t)p[9] << 8));

        int32_t out_amps = (int32_t)((uint32_t)p[10] | ((uint32_t)p[11] << 8)
                         | ((uint32_t)p[12] << 16) | ((uint32_t)p[13] << 24));
        out->output_amps = (double)out_amps * 0.1;

        int32_t field = (int32_t)((uint32_t)p[14] | ((uint32_t)p[15] << 8)
                      | ((uint32_t)p[16] << 16) | ((uint32_t)p[17] << 24));
        out->field_amps = (double)field * 0.01;

        out->throttle_pointer = (int16_t)((uint16_t)p[24] | ((uint16_t)p[25] << 8));
        out->overtemp_cap = (int16_t)((uint16_t)p[40] | ((uint16_t)p[41] << 8));
        out->speed_rpm = (int16_t)((uint16_t)p[42] | ((uint16_t)p[43] << 8));
    }

    /* Block 5: Battery (22 bytes at 0x2000F148) */
    {
        const uint8_t* p = blocks[5];
        int16_t soc = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        out->state_of_charge = (double)soc * 0.1;

        int32_t batt_amps = (int32_t)((uint32_t)p[18] | ((uint32_t)p[19] << 8)
                          | ((uint32_t)p[20] << 16) | ((uint32_t)p[21] << 24));
        out->battery_amps = (double)batt_amps * 0.1;
    }

    return ALLTRAX_OK;
}
