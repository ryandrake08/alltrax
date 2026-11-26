/*
 * write.c — Writes (single CAL/RUN bracket for RAM + FLASH), restore and reset
 */

#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <time.h>

alltrax_error alltrax_reset_device(alltrax_controller* ctrl,
    const alltrax_write_opts* opts)
{
    alltrax_error gate_rc = require_writable_firmware(ctrl,
        opts && opts->skip_fw_check);
    if (gate_rc) return gate_rc;

    /* Record the identity that alltrax_wait_for_reconnect() looks for. */
    uint8_t id[SERIAL_NUMBER_OFFSET + 4];
    alltrax_error rc = read_memory(ctrl, ADDR_CONTROLLER_INFO, sizeof(id), id);
    if (rc) return rc;
    get_string(id, 15, ctrl->reset_model, sizeof(ctrl->reset_model));
    ctrl->reset_serial = get_le32(id + SERIAL_NUMBER_OFFSET);

    uint8_t request[PACKET_SIZE];
    build_reset_request(request);

    /* Send reset — device disconnects immediately, no response */
    rc = transport_write(ctrl, request);
    if (!rc)
        ctrl->reset_sent = true;
    return rc;
}

/* Zero the 20 bytes at 0x40006C00, which include the throttle calibration
 * registers at 0x40006C08 (minima) and 0x40006C0C (maxima). A write echo is
 * the controller reading the memory back after the write. The running
 * firmware keeps rewriting the word at 0x40006C10 (values of 2039-2083 were
 * seen with the keyswitch on), so its echo can differ from the zeros
 * written even though the write succeeded. Only the first 16 bytes, which
 * end with the calibration registers, are compared. */
static alltrax_error clear_backup_ram(alltrax_controller* ctrl)
{
    const uint8_t zeros[BACKUP_RAM_SIZE] = {0};
    return write_memory_echo(ctrl, ADDR_BACKUP_RAM, zeros, sizeof(zeros),
                             BACKUP_RAM_ECHO_CHECKED);
}

alltrax_error alltrax_clear_autocal(alltrax_controller* ctrl,
    const alltrax_write_opts* opts)
{
    alltrax_error rc = require_writable_firmware(ctrl, opts && opts->skip_fw_check);
    if (rc) return rc;
    rc = clear_backup_ram(ctrl);
    if (rc) return rc;
    return alltrax_reset_device(ctrl, opts);
}

/* Pre-encoded write payloads */
typedef struct { size_t offset; uint8_t data[MAX_PAYLOAD]; int len; } flash_patch_t;
typedef struct { uint8_t data[MAX_PAYLOAD]; int len; uint32_t addr; } ram_encoded_t;

#define MAX_RAM_WRITE_VARS    16
#define MAX_FLASH_WRITE_VARS  96

/* Commit a complete page, keeping GoodSet invalid until verification. */
static alltrax_error commit_varset_page(alltrax_controller* ctrl,
    uint8_t page_data[VARSET_SIZE])
{
    /* Set GoodSet to 0xFFFF (write in progress) */
    page_data[0] = 0xFF;
    page_data[1] = 0xFF;

    /* Erase page */
    alltrax_error rc = page_erase(ctrl);
    if (rc) return rc;

    for (size_t offset = 0; offset < VARSET_SIZE; offset += MAX_PAYLOAD) {
        size_t chunk = VARSET_SIZE - offset;
        if (chunk > MAX_PAYLOAD) chunk = MAX_PAYLOAD;
        rc = program_flash(ctrl, VARSET_ADDR + (uint32_t)offset,
                           page_data + offset, chunk);
        if (rc) return rc;
    }

    /* Read-back verification */
    uint8_t readback[VARSET_SIZE];
    rc = read_memory_range(ctrl, VARSET_ADDR, VARSET_SIZE, readback);
    if (rc) return rc;
    for (size_t i = 0; i < VARSET_SIZE; i++) {
        if (readback[i] != page_data[i]) {
            set_error_detail(ctrl,
                "FLASH verification failed at offset 0x%04X: "
                "wrote 0x%02X, read 0x%02X",
                (unsigned)i, page_data[i], readback[i]);
            return ALLTRAX_ERR_FLASH;
        }
    }

    /* Clear GoodSet (mark settings as valid) */
    uint8_t zeros[2] = { 0x00, 0x00 };
    return program_flash(ctrl, VARSET_ADDR, zeros, sizeof(zeros));
}

static alltrax_error read_factory_marker(alltrax_controller* ctrl, uint8_t marker[2])
{
    alltrax_error rc = read_memory(ctrl, ADDR_F_GOODSET, 2, marker);
    if (rc) return rc;
    if (get_le16(marker) != 0) {
        set_error_detail(ctrl, "Factory settings invalid (F_GoodSet=0x%04X, expected 0x0000)",
                         get_le16(marker));
        return ALLTRAX_ERR_FLASH;
    }
    return ALLTRAX_OK;
}

static alltrax_error check_goodset(alltrax_controller* ctrl, bool* recover)
{
    uint8_t marker[2];
    alltrax_error rc = read_factory_marker(ctrl, marker);
    if (rc) return rc;
    rc = read_memory(ctrl, ADDR_V_GOODSET, sizeof(marker), marker);
    if (!rc)
        *recover = get_le16(marker) != 0;
    return rc;
}

static alltrax_error post_write_error(alltrax_controller* ctrl,
    alltrax_error rc, const char* action)
{
    if (rc) {
        char detail[sizeof(ctrl->error_detail)];
        memcpy(detail, ctrl->error_detail, sizeof(detail));
        set_error_detail(ctrl, "Settings written, but %s failed: %.170s",
                         action, detail);
    }
    return rc;
}

static alltrax_error stamp_field_map_date(alltrax_controller* ctrl)
{
    time_t now = time(NULL);
    struct tm date;
    if (now == (time_t)-1 || !localtime_r(&now, &date)) {
        set_error_detail(ctrl, "Local date is unavailable");
        return ALLTRAX_ERR_INVALID_ARG;
    }
    unsigned year = (unsigned)(date.tm_year + 1900);
    uint8_t payload[] = { (uint8_t)year, (uint8_t)(year >> 8),
                          (uint8_t)date.tm_mday, (uint8_t)(date.tm_mon + 1) };
    return write_memory(ctrl, ADDR_FIELD_MAP_DATE, payload, sizeof(payload));
}

/* Settings-page items other than the linearization curve and field-map data
 * (the Field variables and the field and genfield curves). */
static bool requests_reset(const alltrax_assignment* item)
{
    if (item->curve)
        return strcmp(item->curve->name, "linearization") != 0 &&
               strcmp(item->curve->name, "field") != 0 &&
               strcmp(item->curve->name, "genfield") != 0;
    size_t count;
    const alltrax_var_def* field = alltrax_get_var_defs(ALLTRAX_VARS_FIELD, &count);
    bool field_var = item->var >= field && item->var < field + count;
    return item->var->is_flash && !field_var;
}

/* Actions after a successful settings write, in this order: date stamp,
 * calibration clear, reset. A failure stops the remaining actions. */
static alltrax_error post_write(alltrax_controller* ctrl, bool stamp_date,
    bool clear_autocal, bool reset, const alltrax_write_opts* opts,
    alltrax_write_report* report)
{
    if (stamp_date) {
        alltrax_error rc = stamp_field_map_date(ctrl);
        if (rc) return post_write_error(ctrl, rc, "stamping FieldMap_Date");
        report->date_stamped = true;
    }
    if (clear_autocal) {
        alltrax_error rc = clear_backup_ram(ctrl);
        if (rc) return post_write_error(ctrl, rc, "clearing throttle auto-calibration");
        report->autocal_cleared = true;
    }
    if (!reset)
        return ALLTRAX_OK;
    alltrax_error rc = alltrax_reset_device(ctrl, opts);
    if (rc) return post_write_error(ctrl, rc, "resetting the controller");
    report->reset_sent = true;
    return ALLTRAX_OK;
}

static alltrax_error enter_cal(alltrax_controller* ctrl, bool* cal_attempted)
{
    uint8_t mode_buf[1];
    alltrax_error rc = read_memory(ctrl, ADDR_RUN_MODE, 1, mode_buf);
    if (rc) return rc;

    if (mode_buf[0] != RUN_MODE_RUN)
        return ALLTRAX_OK;

    uint8_t cal = RUN_MODE_CAL;
    /* A failed transfer or echo cannot prove that CAL was not applied. */
    *cal_attempted = true;
    rc = write_memory(ctrl, ADDR_RUN_MODE, &cal, 1);
    /* A read-only rejection proves that the command was not sent. */
    if (rc == ALLTRAX_ERR_BLOCKED) *cal_attempted = false;
    return rc;
}

static alltrax_error exit_cal(alltrax_controller* ctrl)
{
    uint8_t run = RUN_MODE_RUN;
    return write_memory(ctrl, ADDR_RUN_MODE, &run, 1);
}

static alltrax_error finish_cal_write(alltrax_controller* ctrl,
    bool cal_attempted, alltrax_error rc)
{
    if (!cal_attempted)
        return rc;

    char original_detail[sizeof(ctrl->error_detail)];
    memcpy(original_detail, ctrl->error_detail, sizeof(original_detail));
    alltrax_error cleanup_rc = exit_cal(ctrl);
    if (!cleanup_rc) {
        if (rc) memcpy(ctrl->error_detail, original_detail, sizeof(original_detail));
        return rc;
    }

    char cleanup_detail[sizeof(ctrl->error_detail)];
    memcpy(cleanup_detail, ctrl->error_detail, sizeof(cleanup_detail));
    if (rc) {
        set_error_detail(ctrl,
            "Operation failed (%d): %.90s; RUN restoration failed (%d): %.90s",
            rc, original_detail, cleanup_rc, cleanup_detail);
    } else {
        set_error_detail(ctrl, "RUN restoration failed (%d): %.210s",
                         cleanup_rc, cleanup_detail);
    }
    return rc ? rc : cleanup_rc;
}

/* Validate and encode one assignment into the RAM or FLASH patch list,
 * without device I/O. */
static alltrax_error encode_assignment(alltrax_controller* ctrl,
    const alltrax_assignment* item, ram_encoded_t* ram, size_t* ram_count,
    flash_patch_t* flash, size_t* flash_count)
{
    if (!item->var == !item->curve) {
        set_error_detail(ctrl, "Each assignment needs exactly one variable or curve");
        return ALLTRAX_ERR_INVALID_ARG;
    }

    if (item->curve) {
        const alltrax_curve_def* def = item->curve;
        alltrax_error rc = validate_curve_def(ctrl, def);
        if (rc) return rc;
        if (!item->curve_data) {
            set_error_detail(ctrl, "Curve %s requires curve data", def->name);
            return ALLTRAX_ERR_INVALID_ARG;
        }
        if (*flash_count + 2 > MAX_FLASH_WRITE_VARS) {
            set_error_detail(ctrl, "Too many FLASH variables (max %d)",
                             MAX_FLASH_WRITE_VARS);
            return ALLTRAX_ERR_INVALID_ARG;
        }
        flash_patch_t* x = &flash[*flash_count];
        flash_patch_t* y = x + 1;
        x->offset = def->x_address - VARSET_ADDR;
        y->offset = def->y_address - VARSET_ADDR;
        x->len = y->len = ALLTRAX_CURVE_POINTS * 2;
        rc = encode_curve_data(def, item->curve_data, x->data, y->data);
        if (rc) {
            set_error_detail(ctrl,
                "Invalid %s curve: points must be finite and encode within raw "
                "X [0,%d], Y [0,%d] with finite positive scales",
                def->name, def->x_raw_max, def->y_raw_max);
            return rc;
        }
        *flash_count += 2;
        return ALLTRAX_OK;
    }

    const alltrax_var_def* var = item->var;
    alltrax_error rc = validate_var_for_io(ctrl, var, true);
    if (rc) return rc;

    bool is_string = var->type == ALLTRAX_TYPE_STRING;
    if (is_string != (item->text != NULL)) {
        set_error_detail(ctrl, is_string
            ? "Variable %s is a string; it requires a text value"
            : "Variable %s is not a string; text values are not allowed",
            var->name);
        return ALLTRAX_ERR_INVALID_ARG;
    }
    if (!is_string && alltrax_validate_var_value(var, item->number)) {
        set_error_detail(ctrl, "Value out of range for %s", var->name);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    uint8_t* data;
    int* len;
    if (var->is_flash) {
        if (*flash_count >= MAX_FLASH_WRITE_VARS) {
            set_error_detail(ctrl, "Too many FLASH variables (max %d)",
                             MAX_FLASH_WRITE_VARS);
            return ALLTRAX_ERR_INVALID_ARG;
        }
        flash_patch_t* p = &flash[*flash_count];
        p->offset = var->address - VARSET_ADDR;
        data = p->data;
        len = &p->len;
    } else {
        if (*ram_count >= MAX_RAM_WRITE_VARS) {
            set_error_detail(ctrl, "Too many RAM variables (max %d)",
                             MAX_RAM_WRITE_VARS);
            return ALLTRAX_ERR_INVALID_ARG;
        }
        ram_encoded_t* r = &ram[*ram_count];
        r->addr = var->address;
        data = r->data;
        len = &r->len;
    }

    *len = is_string ? alltrax_encode_string(var, item->text, data)
                     : alltrax_encode_var(var, item->number, data);
    if (*len <= 0) {
        if (is_string)
            set_error_detail(ctrl,
                "Invalid text for %s: printable ASCII, at most %u characters",
                var->name, (unsigned)var->size);
        else
            set_error_detail(ctrl, "Cannot encode variable %s", var->name);
        return ALLTRAX_ERR_INVALID_ARG;
    }

    if (var->is_flash)
        (*flash_count)++;
    else
        (*ram_count)++;
    return ALLTRAX_OK;
}

static const char* assignment_name(const alltrax_assignment* item)
{
    return item->var ? item->var->name : item->curve->name;
}

/* Byte spans an assignment writes: one for a variable, two for a curve. */
static size_t assignment_spans(const alltrax_assignment* item,
    uint32_t addr[2], size_t len[2])
{
    if (item->curve) {
        addr[0] = item->curve->x_address;
        addr[1] = item->curve->y_address;
        len[0] = len[1] = ALLTRAX_CURVE_POINTS * 2;
        return 2;
    }
    addr[0] = item->var->address;
    len[0] = alltrax_var_byte_size(item->var);
    return 1;
}

/* Reject repeated or overlapping targets. Items must already be validated
 * library entries. */
static alltrax_error check_overlaps(alltrax_controller* ctrl,
    const alltrax_assignment* items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t ai[2], aj[2];
        size_t li[2], lj[2];
        size_t ni = assignment_spans(&items[i], ai, li);
        for (size_t j = i + 1; j < count; j++) {
            size_t nj = assignment_spans(&items[j], aj, lj);
            for (size_t a = 0; a < ni; a++) {
                for (size_t b = 0; b < nj; b++) {
                    if (ai[a] < aj[b] + lj[b] && aj[b] < ai[a] + li[a]) {
                        set_error_detail(ctrl, "%s and %s write the same bytes; "
                                         "each target may appear once per batch",
                                         assignment_name(&items[i]),
                                         assignment_name(&items[j]));
                        return ALLTRAX_ERR_INVALID_ARG;
                    }
                }
            }
        }
    }
    return ALLTRAX_OK;
}

alltrax_error alltrax_write_assignments(alltrax_controller* ctrl,
    const alltrax_assignment* items, size_t count,
    const alltrax_write_opts* opts)
{
    alltrax_write_report unused_report;
    alltrax_write_report* report = opts && opts->report ? opts->report
                                                        : &unused_report;
    memset(report, 0, sizeof(*report));

    alltrax_error gate_rc = require_writable_firmware(ctrl,
        opts && opts->skip_fw_check);
    if (gate_rc) return gate_rc;
    if (!items || count == 0) {
        set_error_detail(ctrl, "Assignments are required");
        return ALLTRAX_ERR_INVALID_ARG;
    }

    /* Ensure we have opts, use defaults if not passed */
    alltrax_write_opts defaults = {0};
    if (!opts) opts = &defaults;

    /* ---- 1. Validate and encode every assignment ---- */

    ram_encoded_t ram_encoded[MAX_RAM_WRITE_VARS];
    flash_patch_t flash_patches[MAX_FLASH_WRITE_VARS];
    size_t ram_count = 0;
    size_t flash_count = 0;
    alltrax_error rc;

    for (size_t i = 0; i < count; i++) {
        rc = encode_assignment(ctrl, &items[i], ram_encoded, &ram_count,
                               flash_patches, &flash_count);
        if (rc) return rc;
    }

    /* ---- 1b. Each target byte appears once per batch ---- */

    rc = check_overlaps(ctrl, items, count);
    if (rc) return rc;

    /* Select the base page before any writes. */
    bool recover = false;
    uint8_t page_data[VARSET_SIZE];
    if (flash_count) {
        rc = check_goodset(ctrl, &recover);
        if (rc) return rc;
        if (recover) {
            rc = read_memory_range(ctrl, FACTSET_ADDR, sizeof(page_data), page_data);
            if (rc) return rc;
        }
    }

    /* ---- 3. Enter CAL mode (single bracket for all writes) ---- */

    bool cal_attempted = false;
    rc = enter_cal(ctrl, &cal_attempted);
    if (rc) return finish_cal_write(ctrl, cal_attempted, rc);

    /* Restore a complete, valid base page before applying the batch. */
    if (recover) {
        rc = commit_varset_page(ctrl, page_data);
        if (rc) goto restore_run;
        report->factory_restored = true;
    }

    /* ---- 4. Write RAM variables ---- */

    for (size_t i = 0; i < ram_count; i++) {
        rc = write_memory(ctrl, ram_encoded[i].addr,
                          ram_encoded[i].data, (size_t)ram_encoded[i].len);
        if (rc) goto restore_run;
    }

    /* ---- 5. FLASH page cycle ---- */

    if (flash_count > 0) {
        rc = read_memory_range(ctrl, VARSET_ADDR, sizeof(page_data), page_data);
        if (rc) goto restore_run;
        for (size_t i = 0; i < flash_count; i++) {
            memcpy(page_data + flash_patches[i].offset, flash_patches[i].data,
                   (size_t)flash_patches[i].len);
        }
        rc = commit_varset_page(ctrl, page_data);
        if (rc) goto restore_run;
    }

    rc = ALLTRAX_OK;

/* Success and write failures restore RUN if CAL was attempted;
 * cleanup preserves any write error. */
restore_run:
    rc = finish_cal_write(ctrl, cal_attempted, rc);
    if (rc) return rc;

    bool stamp_date = false, clear_autocal = false;
    bool reset = report->factory_restored;
    for (size_t i = 0; i < count; i++) {
        reset |= requests_reset(&items[i]);
        if (!items[i].var) continue;
        const char* name = items[i].var->name;
        if (!strcmp(name, "Throttle_Type") || !strcmp(name, "HPD"))
            clear_autocal = true;
        if (!strcmp(name, "F_Table_Name"))
            stamp_date = true;
    }
    return post_write(ctrl, stamp_date, clear_autocal, reset, opts, report);
}

alltrax_error alltrax_restore_defaults(alltrax_controller* ctrl,
    const alltrax_write_opts* opts)
{
    const alltrax_write_opts defaults = {0};
    if (!opts) opts = &defaults;
    alltrax_write_report unused;
    alltrax_write_report* report = opts->report ? opts->report : &unused;
    *report = (alltrax_write_report){0};

    alltrax_error rc = require_writable_firmware(ctrl, opts->skip_fw_check);
    if (rc) return rc;

    uint8_t factory[VARSET_SIZE], saved[VARSET_SIZE], page[VARSET_SIZE];
    rc = read_factory_marker(ctrl, factory);
    if (rc) return rc;
    rc = read_memory_range(ctrl, FACTSET_ADDR + 2, sizeof(factory) - 2, factory + 2);
    if (rc) return rc;
    rc = read_memory_range(ctrl, VARSET_ADDR, sizeof(saved), saved);
    if (rc) return rc;

    uint8_t rev[4];
    const alltrax_var_def* original = alltrax_find_var("OriginalPrgmRev");
    rc = read_memory(ctrl, original->address, sizeof(rev), rev);
    if (rc) return rc;

    /* Build the final page: factory values with fixes for older controllers
     * (an unprogrammed revision reads as -1). */
    memcpy(page, factory, sizeof(page));
    if ((int32_t)get_le32(rev) < 1007) {
        const alltrax_var_def* rate = alltrax_find_var("N_Throttle_Rate");
        const uint8_t* factory_rate = page + (rate->address - VARSET_ADDR);
        const char* const names[] = {
            "UserInputs_State", "U1_Throttle_Rate", "U2_Throttle_Rate",
            "N_Forward_Speed", "N_Forward_Speed_Max", "U1_Forward_Speed",
            "U1_Forward_Speed_Max", "U2_Forward_Speed", "U2_Forward_Speed_Max",
        };
        const uint8_t zero[] = {0, 0};
        const uint8_t full[] = {100, 0};
        const uint8_t* const values[] = {
            zero, factory_rate, factory_rate, full, full, full, full, full, full,
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            const alltrax_var_def* var = alltrax_find_var(names[i]);
            memcpy(page + (var->address - VARSET_ADDR), values[i], 2);
        }
    }

    /* The firmware runs on factory values while the user page is invalid.
     * Learned calibration belongs to the running throttle type and HPD. */
    const uint8_t* running = get_le16(saved) == 0 ? saved : factory;
    size_t type = alltrax_find_var("Throttle_Type")->address - VARSET_ADDR;
    size_t hpd = alltrax_find_var("HPD")->address - VARSET_ADDR;
    bool throttle_changed = page[type] != running[type] || page[hpd] != running[hpd];
    bool patched = memcmp(page, factory, sizeof(page)) != 0;

    /* One verified page write. A plain factory copy runs without a CAL
     * bracket; a patched page is written under CAL like other settings. */
    bool cal_attempted = false;
    if (patched)
        rc = enter_cal(ctrl, &cal_attempted);
    if (!rc)
        rc = commit_varset_page(ctrl, page);
    rc = finish_cal_write(ctrl, cal_attempted, rc);
    if (rc) return rc;
    report->factory_restored = true;
    return post_write(ctrl, true, throttle_changed, true, opts, report);
}

alltrax_error alltrax_write_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
    const alltrax_write_opts* opts)
{
    alltrax_assignment items[MAX_RAM_WRITE_VARS + MAX_FLASH_WRITE_VARS];
    if (!vars || !values)
        count = 0;
    if (count > sizeof(items) / sizeof(items[0])) {
        if (ctrl) set_error_detail(ctrl, "Too many variables (max %zu)",
                                   sizeof(items) / sizeof(items[0]));
        return ALLTRAX_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++)
        items[i] = (alltrax_assignment){ .var = vars[i], .number = values[i] };
    return alltrax_write_assignments(ctrl, items, count, opts);
}

alltrax_error alltrax_write_curve(alltrax_controller* ctrl,
    const alltrax_curve_def* def, const alltrax_curve_data* data,
    const alltrax_write_opts* opts)
{
    const alltrax_assignment item = { .curve = def, .curve_data = data };
    return alltrax_write_assignments(ctrl, &item, 1, opts);
}
