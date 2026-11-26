/*
 * read.c — Variable, curve, field-map history, memory and monitor reads
 */

#include "internal.h"

/* ------------------------------------------------------------------ */
/* Variable read (with block coalescing)                              */
/* ------------------------------------------------------------------ */

/* Gap threshold: merge consecutive variables whose addresses are within
 * this many bytes of each other. */
#define COALESCE_GAP 20

/* Maximum variables in a single alltrax_read_vars() call */
#define MAX_READ_VARS 256
static alltrax_error read_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out,
    bool factory)
{
    alltrax_error gate_rc = require_readable_firmware(ctrl);
    if (gate_rc) return gate_rc;
    if (count == 0)
        return ALLTRAX_OK;

    if (count > MAX_READ_VARS) {
        set_error_detail(ctrl, "Too many variables to read (%zu, max %d)",
                         count, MAX_READ_VARS);
        return ALLTRAX_ERR_INVALID_ARG;
    }
    if (!vars || !out)
        return ALLTRAX_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; i++) {
        alltrax_error rc = validate_var_for_io(ctrl, vars[i], false);
        if (rc) return rc;
        if (factory && (!vars[i]->is_flash ||
            !address_span_fits(vars[i]->address, alltrax_var_byte_size(vars[i]),
                               VARSET_ADDR, VARSET_SIZE))) {
            set_error_detail(ctrl, "%s has no user settings page offset", vars[i]->name);
            return ALLTRAX_ERR_INVALID_ARG;
        }
    }

    /* Build sorted index array (indices into vars[]/out[]) */
    size_t idx[MAX_READ_VARS];
    for (size_t i = 0; i < count; i++)
        idx[i] = i;

    /* Insertion sort by address (small N, no stdlib dependency) */
    for (size_t i = 1; i < count; i++) {
        size_t key = idx[i];
        uint32_t key_addr = vars[key]->address;
        size_t j = i;
        while (j > 0 && vars[idx[j - 1]]->address > key_addr) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = key;
    }

    /* Scan sorted variables, forming coalesced blocks */
    size_t pos = 0;
    while (pos < count) {
        size_t block_start = pos;
        uint32_t base_addr = vars[idx[pos]]->address;
        uint32_t block_end = base_addr + (uint32_t)alltrax_var_byte_size(vars[idx[pos]]);

        /* Extend block with consecutive variables */
        while (pos + 1 < count) {
            size_t next = idx[pos + 1];
            uint32_t next_addr = vars[next]->address;
            uint32_t next_end = next_addr + (uint32_t)alltrax_var_byte_size(vars[next]);

            /* Gap too large or block would exceed MAX_PAYLOAD? */
            if (next_addr > block_end + COALESCE_GAP)
                break;
            if (next_end - base_addr > MAX_PAYLOAD)
                break;

            block_end = next_end > block_end ? next_end : block_end;
            pos++;
        }
        pos++; /* advance past last variable in this block */

        /* Read the coalesced block */
        size_t span = block_end - base_addr;
        uint8_t buf[MAX_PAYLOAD];
        uint32_t read_addr = factory ? FACTSET_ADDR + (base_addr - VARSET_ADDR)
                                     : base_addr;
        alltrax_error rc = read_memory(ctrl, read_addr, span, buf);
        if (rc) return rc;

        /* Decode each variable from the block buffer */
        for (size_t k = block_start; k < pos; k++) {
            size_t orig = idx[k];
            rc = alltrax_decode_var(buf, span, vars[orig], base_addr,
                                    &out[orig]);
            if (rc) return rc;
        }
    }

    return ALLTRAX_OK;
}

alltrax_error alltrax_read_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out)
{
    return read_vars(ctrl, vars, count, out, false);
}

alltrax_error alltrax_read_factory_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out)
{
    return read_vars(ctrl, vars, count, out, true);
}

/* ------------------------------------------------------------------ */
/* Curve read                                                          */
/* ------------------------------------------------------------------ */

/* Read a 16-point int16 array from FLASH, scale to display units */
static alltrax_error read_curve_array(alltrax_controller* ctrl,
    uint32_t addr, double scale, double* out)
{
    uint8_t buf[ALLTRAX_CURVE_POINTS * 2];
    alltrax_error rc = read_memory(ctrl, addr,
        ALLTRAX_CURVE_POINTS * 2, buf);
    if (rc) return rc;

    for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++)
        out[i] = (double)(int16_t)get_le16(buf + i * 2) * scale;

    return ALLTRAX_OK;
}

alltrax_error alltrax_read_curve(alltrax_controller* ctrl,
    const alltrax_curve_def* def, alltrax_curve_data* out)
{
    alltrax_error gate_rc = require_readable_firmware(ctrl);
    if (gate_rc) return gate_rc;
    if (!out)
        return ALLTRAX_ERR_INVALID_ARG;
    alltrax_error rc = validate_curve_def(ctrl, def);
    if (rc) return rc;

    out->def = def;

    rc = read_curve_array(ctrl, def->x_address, def->x_scale, out->x);
    if (rc) return rc;

    return read_curve_array(ctrl, def->y_address, def->y_scale, out->y);
}

alltrax_error alltrax_read_curve_factory(alltrax_controller* ctrl,
    const alltrax_curve_def* def, alltrax_curve_data* out)
{
    alltrax_error gate_rc = require_readable_firmware(ctrl);
    if (gate_rc) return gate_rc;
    if (!out)
        return ALLTRAX_ERR_INVALID_ARG;
    alltrax_error rc = validate_curve_def(ctrl, def);
    if (rc) return rc;

    out->def = def;

    rc = read_curve_array(ctrl, def->factory_x_address, def->x_scale, out->x);
    if (rc) return rc;

    return read_curve_array(ctrl, def->factory_y_address,
                            def->y_scale, out->y);
}

/* ------------------------------------------------------------------ */
/* Field-map history                                                   */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_read_field_map_history(alltrax_controller* ctrl,
    alltrax_field_map_history* out)
{
    if (!out) return ALLTRAX_ERR_INVALID_ARG;
    alltrax_error rc = require_readable_firmware(ctrl);
    if (rc) return rc;
    if (ctrl->is_srx) {
        set_error_detail(ctrl, "Field-map history is unavailable on SRX controllers");
        return ALLTRAX_ERR_UNSUPPORTED;
    }
    if (ctrl->program_rev <= 2002) {
        set_error_detail(ctrl, "Field-map history requires program firmware newer than V2.002");
        return ALLTRAX_ERR_FIRMWARE;
    }

    uint8_t page[DATA_SIZE];
    rc = read_memory_range(ctrl, DATA_ADDR, sizeof(page), page);
    if (rc) return rc;

    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < ALLTRAX_FIELD_MAP_HISTORY_MAX_RECORDS; i++) {
        const uint8_t* entry = page + i * 36;
        if (get_le32(entry) == UINT32_MAX) break;
        alltrax_field_map_history_record* record = &out->records[i];
        record->year = get_le16(entry);
        record->day = entry[2];
        record->month = entry[3];
        get_string(entry + 4, 32, record->name, sizeof(record->name));
        out->record_count++;
    }

    uint32_t rollovers = get_le16(page + sizeof(page) - 2);
    if (rollovers == UINT16_MAX) rollovers = 0;
    /* Each rollover stands for 46 older records no longer on the page. */
    out->map_count = rollovers * 46 + (uint32_t)out->record_count;
    return ALLTRAX_OK;
}

bool alltrax_memory_readable(uint32_t address, size_t length)
{
    return length <= ALLTRAX_READ_MEMORY_MAX &&
           (address_span_fits(address, length, RAM_BASE, RAM_END - RAM_BASE) ||
            address_span_fits(address, length, FLASH_BASE, FLASH_END - FLASH_BASE) ||
            address_span_fits(address, length, ADDR_BACKUP_RAM, BACKUP_RAM_SIZE));
}

alltrax_error alltrax_read_memory(alltrax_controller* ctrl, uint32_t address,
    size_t length, uint8_t* out)
{
    if (!out) return ALLTRAX_ERR_INVALID_ARG;
    alltrax_error rc = require_readable_firmware(ctrl);
    if (rc) return rc;
    if (!alltrax_memory_readable(address, length)) {
        set_error_detail(ctrl, "Cannot read %zu bytes at 0x%08X: reads are limited "
                         "to RAM, FLASH and the backup registers", length, address);
        return ALLTRAX_ERR_INVALID_ARG;
    }
    return read_memory_range(ctrl, address, length, out);
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
    { 0x2000F148, 30 },  /* Battery, power, user profile and inputs */
};
#define MONITOR_BLOCK_COUNT 6

alltrax_error alltrax_read_monitor(alltrax_controller* ctrl,
    alltrax_monitor_data* out)
{
    alltrax_error gate_rc = require_readable_firmware(ctrl);
    if (gate_rc) return gate_rc;
    memset(out, 0, sizeof(*out));

    uint8_t blocks[MONITOR_BLOCK_COUNT][MAX_PAYLOAD];
    for (int i = 0; i < MONITOR_BLOCK_COUNT; i++) {
        alltrax_error rc = read_memory(ctrl,
            monitor_blocks[i].addr, monitor_blocks[i].size,
            blocks[i]);
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
    out->bad_vars_code = get_le16(blocks[2] + 6); /* 0x2000F096 */
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

    /* Block 5: Battery and user inputs (30 bytes at 0x2000F148) */
    {
        const uint8_t* p = blocks[5];
        out->state_of_charge     = (double)(int16_t)get_le16(p + 2) * 0.1;
        out->battery_amps        = (double)(int32_t)get_le32(p + 18) * 0.1;
        out->user_profile_num    = (int16_t)get_le16(p + 24); /* 0x2000F160 */
        out->user1_input_percent = (int16_t)get_le16(p + 26); /* 0x2000F162 */
        out->user2_input_percent = (int16_t)get_le16(p + 28); /* 0x2000F164 */
    }

    return ALLTRAX_OK;
}
