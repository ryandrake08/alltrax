/*
 * controller.c — High-level operations: read/write/flash/reset
 */

#include "internal.h"
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Memory read/write                                                   */
/* ------------------------------------------------------------------ */

static alltrax_error read_memory(alltrax_controller* ctrl, uint32_t addr,
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
    rc = transport_read(ctrl, response);
    if (rc) return rc;

    uint8_t result, n;
    uint8_t payload[MAX_PAYLOAD];
    rc = parse_response(response, RESPONSE_TYPE_RW, &result, &n, payload);
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

static alltrax_error read_memory_range(alltrax_controller* ctrl, uint32_t addr,
    size_t total_bytes, uint8_t* buf)
{
    size_t offset = 0;
    while (offset < total_bytes) {
        size_t chunk = total_bytes - offset;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;

        size_t got;
        alltrax_error rc = read_memory(ctrl, addr + (uint32_t)offset,
                                                chunk, buf + offset, &got);
        if (rc) return rc;
        offset += chunk;
    }
    return ALLTRAX_OK;
}

static alltrax_error write_memory(alltrax_controller* ctrl, uint32_t addr,
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
    rc = transport_read(ctrl, response);
    if (rc) return rc;

    uint8_t result, n;
    uint8_t echo[MAX_PAYLOAD];
    rc = parse_response(response, RESPONSE_TYPE_RW, &result, &n, echo);
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

        alltrax_error rc = write_memory(ctrl, addr + (uint32_t)offset,
                                                 data + offset, chunk);
        if (rc) return rc;
        offset += chunk;
    }
    return ALLTRAX_OK;
}

/* ------------------------------------------------------------------ */
/* Controller info                                                     */
/* ------------------------------------------------------------------ */

static char* format_rev(uint32_t rev, char* buf, size_t buflen)
{
    uint32_t major = rev / 1000;
    uint32_t minor = rev % 1000;
    snprintf(buf, buflen, "V%u.%03u", major, minor);
    return buf;
}

alltrax_error alltrax_get_info(alltrax_controller* ctrl, alltrax_info* out)
{
    if (!out)
        return ALLTRAX_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    alltrax_error rc;
    uint8_t buf[56];

    /* Controller identity (52 bytes from 0x08000800) */
    rc = read_memory(ctrl, ADDR_CONTROLLER_INFO, 52, buf, NULL);
    if (rc) return rc;

    get_string(buf, 15, out->model, sizeof(out->model));
    get_string(buf + 0x10, 15, out->build_date, sizeof(out->build_date));
    out->serial_number        = get_le32(buf + 0x20);
    out->original_boot_rev    = get_le32(buf + 0x24);
    out->original_program_rev = get_le32(buf + 0x28);
    out->program_type         = get_le32(buf + 0x2C);
    out->hardware_rev         = get_le32(buf + 0x30);

    /* Hardware config (24 bytes from 0x08000880) */
    rc = read_memory(ctrl, ADDR_HARDWARE_CONFIG, 24, buf, NULL);
    if (rc) return rc;

    out->rated_voltage     = get_le16(buf);
    out->rated_amps        = get_le16(buf + 2);
    out->rated_field_amps  = get_le16(buf + 4);
    out->speed_sensor      = buf[6] != 0;
    /* buf[7] is padding */
    out->has_bms_can       = buf[8] != 0;
    out->has_throt_can     = buf[9] != 0;
    out->has_user2         = buf[10] != 0;
    out->has_user3         = buf[11] != 0;
    out->has_aux_out1      = buf[12] != 0;
    out->has_aux_out2      = buf[13] != 0;
    /* buf[14..15] is padding */
    out->throttles_allowed = get_le32(buf + 16);
    out->has_forward       = buf[20] != 0;
    out->has_user1         = buf[21] != 0;
    out->can_high_side     = buf[22] != 0;
    out->is_stock_mode     = buf[23] != 0;

    /* Read boot revision (4 bytes) */
    uint8_t boot_buf[4];
    rc = read_memory(ctrl, ADDR_BOOT_REV, 4, boot_buf, NULL);
    if (rc) return rc;
    out->boot_rev = get_le32(boot_buf);

    /* Read program revision + version (6 bytes) */
    uint8_t prgm_buf[6];
    rc = read_memory(ctrl, ADDR_PRGM_REV, 6, prgm_buf, NULL);
    if (rc) return rc;
    out->program_rev = get_le32(prgm_buf);
    out->program_ver = get_le16(prgm_buf + 4);

    /* Format revision strings */
    format_rev(out->boot_rev, out->boot_rev_str,
               sizeof(out->boot_rev_str));
    format_rev(out->original_boot_rev, out->original_boot_rev_str,
               sizeof(out->original_boot_rev_str));
    format_rev(out->program_rev, out->program_rev_str,
               sizeof(out->program_rev_str));
    format_rev(out->original_program_rev, out->original_program_rev_str,
               sizeof(out->original_program_rev_str));

    /* Reject firmware versions outside the Toolkit's known range */
    if (out->program_rev < 1 || out->program_rev >= 6000) {
        char rev_str[16];
        format_rev(out->program_rev, rev_str, sizeof(rev_str));
        set_error_detail(ctrl,
            "Unsupported firmware %s (expected V0.001-V5.999)", rev_str);
        return ALLTRAX_ERR_FIRMWARE;
    }

    return ALLTRAX_OK;
}

bool alltrax_has_feature(const alltrax_info* info, alltrax_feature feat)
{
    uint32_t orig = info->original_program_rev;
    uint32_t prgm = info->program_rev;

    switch (feat) {
    case ALLTRAX_FEAT_THROTTLE_CAPS:  return orig > 2;
    case ALLTRAX_FEAT_FORWARD_INPUT:  return orig >= 68;
    case ALLTRAX_FEAT_USER1_INPUT:    return orig >= 70;
    case ALLTRAX_FEAT_USER_PROFILES:  return prgm >= 1005;
    case ALLTRAX_FEAT_USER_DEFAULTS:  return orig >= 1007;
    case ALLTRAX_FEAT_CAN_HIGHSIDE:   return orig >= 1008;
    case ALLTRAX_FEAT_BAD_VARS_CODE:  return prgm >= 1107;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Variable read                                                      */
/* ------------------------------------------------------------------ */

static alltrax_error read_var(alltrax_controller* ctrl,
    const alltrax_var_def* var, alltrax_var_value* out)
{
    size_t var_size = alltrax_var_byte_size(var);
    if (var_size == 0 || var_size > MAX_PAYLOAD) {
        set_error_detail(ctrl, "Invalid variable size: %zu", var_size);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    uint8_t buf[MAX_PAYLOAD];
    size_t got;
    alltrax_error rc = read_memory(ctrl, var->address, var_size,
                                            buf, &got);
    if (rc) return rc;

    return alltrax_decode_var(buf, got, var, var->address, out);
}

alltrax_error alltrax_read_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out)
{
    for (size_t i = 0; i < count; i++) {
        alltrax_error rc = read_var(ctrl, vars[i], &out[i]);
        if (rc) return rc;
    }
    return ALLTRAX_OK;
}

/* ------------------------------------------------------------------ */
/* Variable write (single CAL/RUN bracket for RAM + FLASH)             */
/* ------------------------------------------------------------------ */

static alltrax_error page_erase(alltrax_controller* ctrl, uint8_t page)
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
    rc = transport_read(ctrl, response);
    if (rc) return rc;

    uint8_t result, n;
    uint8_t payload[MAX_PAYLOAD];
    rc = parse_response(response, RESPONSE_TYPE_SP, &result, &n, payload);
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

/* Pre-encoded write payloads */
typedef struct { size_t offset; uint8_t data[MAX_PAYLOAD]; int len; } flash_patch_t;
typedef struct { uint8_t data[MAX_PAYLOAD]; int len; uint32_t addr; } ram_encoded_t;

#define MAX_RAM_WRITE_VARS    16
#define MAX_FLASH_WRITE_VARS  96

/* FLASH page cycle: read page → patch → erase → write → verify → clear GoodSet.
 * Called from within the CAL/RUN bracket in alltrax_write_vars(). */

static alltrax_error write_flash_page(alltrax_controller* ctrl,
    const flash_patch_t* patches, size_t count, bool skip_verify)
{
    uint8_t page_data[VARSET_SIZE];

    /* Read full VARSET page (2048 bytes) */
    alltrax_error rc = read_memory_range(ctrl, VARSET_ADDR, VARSET_SIZE,
                                         page_data);
    if (rc) return rc;

    /* Patch all FLASH variables into page buffer */
    for (size_t i = 0; i < count; i++) {
        memcpy(page_data + patches[i].offset,
               patches[i].data, (size_t)patches[i].len);
    }

    /* Set GoodSet to 0xFFFF (write in progress) */
    page_data[0] = 0xFF;
    page_data[1] = 0xFF;

    /* Erase page */
    rc = page_erase(ctrl, VARSET_PAGE);
    if (rc) return rc;

    /* Write full page back */
    rc = write_memory_range(ctrl, VARSET_ADDR, page_data, VARSET_SIZE);
    if (rc) return rc;

    /* Read-back verification */
    if (!skip_verify) {
        uint8_t readback[VARSET_SIZE];

        rc = read_memory_range(ctrl, VARSET_ADDR, VARSET_SIZE, readback);
        if (rc) return rc;

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
            return ALLTRAX_ERR_FLASH;
        }
    }

    /* Clear GoodSet (mark settings as valid) */
    uint8_t zeros[2] = { 0x00, 0x00 };
    return write_memory(ctrl, VARSET_ADDR, zeros, 2);
}

alltrax_error alltrax_write_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
    const alltrax_write_opts* opts)
{    
    if (count == 0) {
        set_error_detail(ctrl, "No variables to write");
        return ALLTRAX_ERR_INVALID_ARG;
    }

    /* Ensure we have opts, use defaults if not passed */
    alltrax_write_opts defaults = {0};
    if (!opts) opts = &defaults;

    /* ---- 1. Validate and count all variables ---- */

    size_t ram_total = 0;
    size_t flash_total = 0;

    for (size_t i = 0; i < count; i++) {
        const alltrax_var_def* var = vars[i];

        if (alltrax_validate_var_value(var, values[i])) {
            set_error_detail(ctrl, "Value out of range for %s", var->name);
            return ALLTRAX_ERR_INVALID_ARG;
        }

        if (var->is_flash) {
            if (var->address < VARSET_ADDR ||
                var->address >= VARSET_ADDR + VARSET_SIZE) {
                set_error_detail(ctrl,
                    "Variable %s at 0x%08X is not in VARSET page "
                    "(0x%08X-0x%08X)",
                    var->name, var->address, VARSET_ADDR,
                    VARSET_ADDR + VARSET_SIZE);
                return ALLTRAX_ERR_ADDRESS;
            }
            flash_total++;
        } else {
            if (var->address < RAM_BASE || var->address >= RAM_END) {
                set_error_detail(ctrl,
                    "Address 0x%08X is outside RAM range (0x%08X-0x%08X)",
                    var->address, RAM_BASE, RAM_END);
                return ALLTRAX_ERR_ADDRESS;
            }
            ram_total++;
        }
    }

    if (ram_total > MAX_RAM_WRITE_VARS) {
        set_error_detail(ctrl, "Too many RAM variables (max %d)",
                         MAX_RAM_WRITE_VARS);
        return ALLTRAX_ERR_INVALID_ARG;
    }
    if (flash_total > MAX_FLASH_WRITE_VARS) {
        set_error_detail(ctrl, "Too many FLASH variables (max %d)",
                         MAX_FLASH_WRITE_VARS);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    /* ---- 2. Encode all variables ---- */

    ram_encoded_t ram_encoded[MAX_RAM_WRITE_VARS];
    flash_patch_t flash_patches[MAX_FLASH_WRITE_VARS];
    size_t ram_count = 0;
    size_t flash_count = 0;

    for (size_t i = 0; i < count; i++) {
        const alltrax_var_def* var = vars[i];

        if (var->is_flash) {
            flash_patch_t* p = &flash_patches[flash_count];
            p->offset = var->address - VARSET_ADDR;
            p->len = alltrax_encode_var(var, values[i], p->data);
            if (p->len <= 0) {
                set_error_detail(ctrl, "Cannot encode variable %s", var->name);
                return ALLTRAX_ERR_INVALID_ARG;
            }
            if (p->offset + (size_t)p->len > VARSET_SIZE) {
                set_error_detail(ctrl,
                    "Variable %s extends beyond VARSET page", var->name);
                return ALLTRAX_ERR_ADDRESS;
            }
            flash_count++;
        } else {
            ram_encoded_t* r = &ram_encoded[ram_count];
            r->addr = var->address;
            r->len = alltrax_encode_var(var, values[i], r->data);
            if (r->len <= 0) {
                set_error_detail(ctrl, "Cannot encode variable %s", var->name);
                return ALLTRAX_ERR_INVALID_ARG;
            }
            ram_count++;
        }
    }

    /* ---- 3. FLASH pre-checks (before entering CAL) ---- */

    alltrax_error rc;

    if (flash_count > 0) {
        if (!opts->skip_fw_check) {
            uint8_t prgm_buf[6];
            rc = read_memory(ctrl, ADDR_PRGM_REV, 6, prgm_buf, NULL);
            if (rc) return rc;

            uint32_t prgm_rev = get_le32(prgm_buf);
            if (prgm_rev != VALIDATED_FIRMWARE) {
                char buf[16];
                format_rev(prgm_rev, buf, sizeof(buf));
                set_error_detail(ctrl,
                    "Firmware %s is not validated. Only V5.005 is supported",
                    buf);
                return ALLTRAX_ERR_FIRMWARE;
            }
        }

        if (!opts->skip_goodset) {
            uint8_t gs_buf[2];
            rc = read_memory(ctrl, ADDR_V_GOODSET, 2, gs_buf, NULL);
            if (rc) return rc;
            uint16_t v_gs = get_le16(gs_buf);

            if (v_gs != 0x0000) {
                set_error_detail(ctrl,
                    "User settings invalid "
                    "(V_GoodSet=0x%04X, expected 0x0000). "
                    "Previous write may have been interrupted", v_gs);
                return ALLTRAX_ERR_FLASH;
            }
        }
    }

    /* ---- 4. Enter CAL mode (single bracket for all writes) ---- */

    bool in_cal = false;

    if (!opts->skip_cal) {
        uint8_t mode_buf[1];
        rc = read_memory(ctrl, ADDR_RUN_MODE, 1, mode_buf, NULL);
        if (rc) return rc;

        if (mode_buf[0] != RUN_MODE_RUN) {
            set_error_detail(ctrl,
                "Controller not in RUN mode (RunMode=0x%02X)", mode_buf[0]);
            return ALLTRAX_ERR_NOT_RUN;
        }

        uint8_t cal = RUN_MODE_CAL;
        rc = write_memory(ctrl, ADDR_RUN_MODE, &cal, 1);
        if (rc) return rc;
        in_cal = true;
    }

    /* ---- 5. Write RAM variables ---- */

    for (size_t i = 0; i < ram_count; i++) {
        rc = write_memory(ctrl, ram_encoded[i].addr,
                          ram_encoded[i].data, (size_t)ram_encoded[i].len);
        if (rc) goto restore_run;
    }

    /* ---- 6. FLASH page cycle ---- */

    if (flash_count > 0) {
        rc = write_flash_page(ctrl, flash_patches, flash_count,
                              opts->skip_verify);
        if (rc) goto restore_run;
    }

    rc = ALLTRAX_OK;

restore_run:
    if (in_cal) {
        uint8_t run = RUN_MODE_RUN;
        alltrax_error rc2 = write_memory(ctrl, ADDR_RUN_MODE, &run, 1);
        if (!rc) rc = rc2;
    }
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
        alltrax_error rc = read_memory(ctrl,
            monitor_blocks[i].addr, monitor_blocks[i].size,
            blocks[i], NULL);
        if (rc) return rc;
    }

    /* Block 0: Error flags (17 bytes) */
    memcpy(out->errors, blocks[0], 17);

    /* Block 1: Error history (17 x uint16) */
    for (int i = 0; i < 17; i++)
        out->error_history[i] = get_le16(blocks[1] + i * 2);

    /* Block 2: Digital inputs (17 bytes at 0x2000F090) */
    out->keyswitch   = blocks[2][0] != 0;   /* 0x2000F090 */
    out->reverse     = blocks[2][1] != 0;   /* 0x2000F091 */
    out->relay_on    = (int16_t)get_le16(blocks[2] + 2);  /* 0x2000F092 */
    out->fan_status  = blocks[2][4] != 0;   /* 0x2000F094 */
    out->hpd_ran     = blocks[2][5] != 0;   /* 0x2000F095 */
    out->bad_vars_code = (int16_t)get_le16(blocks[2] + 6); /* 0x2000F096 */
    out->footswitch  = blocks[2][8] != 0;   /* 0x2000F098 */
    out->forward     = blocks[2][9] != 0;   /* 0x2000F099 */
    out->user1_input = blocks[2][10] != 0;  /* 0x2000F09A */
    out->user2_input = blocks[2][11] != 0;  /* 0x2000F09B */
    out->user3_input = blocks[2][12] != 0;  /* 0x2000F09C */
    out->charger     = blocks[2][13] != 0;  /* 0x2000F09D */
    out->blower      = blocks[2][14] != 0;  /* 0x2000F09E */
    out->check_motor = blocks[2][15] != 0;  /* 0x2000F09F */
    out->horn        = blocks[2][16] != 0;  /* 0x2000F0A0 */

    /* Block 3: Temperature (2 bytes at 0x2000F0E6) */
    out->temp_c = ((double)(int16_t)get_le16(blocks[3]) - 527.0) * 0.1289;

    /* Block 4: Power (56 bytes at 0x2000F110) */
    {
        const uint8_t* p = blocks[4];
        out->battery_volts     = (double)(int16_t)get_le16(p) * 0.1;
        out->throttle_local    = (int16_t)get_le16(p + 6);
        out->throttle_position = (int16_t)get_le16(p + 8);
        out->output_amps       = (double)(int32_t)get_le32(p + 10) * 0.1;
        out->field_amps        = (double)(int32_t)get_le32(p + 14) * 0.01;
        out->throttle_pointer  = (int16_t)get_le16(p + 24);
        out->overtemp_cap      = (int16_t)get_le16(p + 40);
        out->speed_rpm         = (int16_t)get_le16(p + 42);
    }

    /* Block 5: Battery (22 bytes at 0x2000F148) */
    {
        const uint8_t* p = blocks[5];
        out->state_of_charge = (double)(int16_t)get_le16(p + 2) * 0.1;
        out->battery_amps    = (double)(int32_t)get_le32(p + 18) * 0.1;
    }

    return ALLTRAX_OK;
}
