/*
 * identity.c — Controller identity, info and firmware gates
 */

#include "internal.h"

/* ------------------------------------------------------------------ */
/* Controller type detection                                           */
/* ------------------------------------------------------------------ */

/*
 * Determine controller type from the model string prefix.
 *   BMS2  → BMS2  (4-char prefix, checked first)
 *   SPB   → SPM   (SPB is an SPM variant)
 *   SRX   → XCT   (SRX uses XCT protocol)
 *   NCT   → XCT   (NCT uses XCT protocol)
 *   SR*   → SR    (2-char prefix, SPM variant with different fw bounds)
 *   SPM   → SPM
 *   XCT   → XCT
 *   BMS   → BMS
 */
alltrax_controller_type detect_controller_type(const char* model)
{
    size_t len = strlen(model);

    /* BMS2 checked first (4-char prefix) */
    if (len >= 4 && strncmp(model, "BMS2", 4) == 0)
        return ALLTRAX_CONTROLLER_BMS2;

    if (len < 3)
        return ALLTRAX_CONTROLLER_UNKNOWN;

    /* 3-char prefix checks */
    if (strncmp(model, "XCT", 3) == 0)
        return ALLTRAX_CONTROLLER_XCT;
    if (strncmp(model, "SRX", 3) == 0 || strncmp(model, "NCT", 3) == 0)
        return ALLTRAX_CONTROLLER_XCT;
    if (strncmp(model, "SPM", 3) == 0 || strncmp(model, "SPB", 3) == 0)
        return ALLTRAX_CONTROLLER_SPM;
    if (strncmp(model, "BMS", 3) == 0)
        return ALLTRAX_CONTROLLER_BMS;

    /* SR* (2-char prefix) → SR variant */
    if (len >= 2 && model[0] == 'S' && model[1] == 'R')
        return ALLTRAX_CONTROLLER_SR;

    return ALLTRAX_CONTROLLER_UNKNOWN;
}

alltrax_error identify_controller(alltrax_controller* ctrl)
{
    alltrax_error rc = require_xct_usb(ctrl);
    if (rc) return rc;

    ctrl->controller_type = ALLTRAX_CONTROLLER_UNKNOWN;
    uint8_t buf[15];
    rc = read_memory(ctrl, ADDR_CONTROLLER_INFO, sizeof(buf), buf);
    if (rc) return rc;

    char model[16];
    get_string(buf, sizeof(buf), model, sizeof(model));
    ctrl->controller_type = detect_controller_type(model);
    ctrl->is_srx = strstr(model, "SRX") != NULL;
    rc = require_xct_controller(ctrl);
    if (rc) {
        set_error_detail(ctrl, "Unsupported controller model '%s'; "
                         "requires XCT, SRX, or NCT", model);
        return rc;
    }

    uint8_t rev[4];
    rc = read_memory(ctrl, ADDR_BOOT_REV, sizeof(rev), rev);
    if (rc) return rc;
    ctrl->boot_rev = get_le32(rev);
    rc = read_memory(ctrl, ADDR_PRGM_REV, sizeof(rev), rev);
    if (rc) return rc;
    ctrl->program_rev = get_le32(rev);
    uint8_t startup[2];
    rc = read_memory(ctrl, SWITCH_ADDR, sizeof(startup), startup);
    if (!rc)
        ctrl->bootloader_mode = get_le16(startup) != 0;
    return rc;
}

const char* alltrax_controller_type_name(alltrax_controller_type type)
{
    switch (type) {
    case ALLTRAX_CONTROLLER_XCT:     return "XCT";
    case ALLTRAX_CONTROLLER_SPM:     return "SPM";
    case ALLTRAX_CONTROLLER_SR:      return "SR";
    case ALLTRAX_CONTROLLER_BMS:     return "BMS";
    case ALLTRAX_CONTROLLER_BMS2:    return "BMS2";
    case ALLTRAX_CONTROLLER_UNKNOWN: return "Unknown";
    }
    return "Unknown";
}

/* ------------------------------------------------------------------ */
/* Controller info                                                     */
/* ------------------------------------------------------------------ */

char* format_rev(uint32_t rev, char* buf, size_t buflen)
{
    uint32_t major = rev / 1000;
    uint32_t minor = rev % 1000;
    snprintf(buf, buflen, "V%u.%03u", major, minor);
    return buf;
}

/*
 * Check firmware version bounds for the given controller type.
 * Returns true if the firmware is within a good range.
 *
 * Bounds:
 *   XCT: boot 0-5999, program 0-5999
 *   SPM: boot 1050-2999, program 1050-2999
 *   SR:  boot <1000, program <2000
 *   BMS: boot <1000, program <4000
 *   BMS2: (same as BMS)
 */
bool firmware_in_bounds(alltrax_controller_type type,
    uint32_t boot_rev, uint32_t program_rev)
{
    switch (type) {
    case ALLTRAX_CONTROLLER_XCT:
        return program_rev >= 1 && program_rev < 6000 &&
               boot_rev < 6000;
    case ALLTRAX_CONTROLLER_SPM:
        return boot_rev >= 1050 && boot_rev < 3000 &&
               program_rev >= 1050 && program_rev < 3000;
    case ALLTRAX_CONTROLLER_SR:
        return boot_rev < 1000 && program_rev < 2000;
    case ALLTRAX_CONTROLLER_BMS:
    case ALLTRAX_CONTROLLER_BMS2:
        return boot_rev < 1000 && program_rev < 4000;
    case ALLTRAX_CONTROLLER_UNKNOWN:
        return false;
    }
    return false;
}

/* XCT program revisions validated for writes on real hardware. */
static const uint32_t write_validated_firmware[] = { 5005 };

static bool firmware_write_validated(uint32_t program_rev)
{
    size_t n = sizeof(write_validated_firmware) /
               sizeof(write_validated_firmware[0]);
    for (size_t i = 0; i < n; i++) {
        if (write_validated_firmware[i] == program_rev)
            return true;
    }
    return false;
}

/* Reads require firmware within V0.001-V5.999 unless the read override
 * is enabled. */
static alltrax_error require_program_mode(alltrax_controller* ctrl)
{
    alltrax_error rc = require_xct_controller(ctrl);
    if (rc) return rc;
    if (ctrl->bootloader_mode) {
        set_error_detail(ctrl, "Controller is in bootloader mode (StartUpSwitch "
                         "is nonzero); only info is available");
        return ALLTRAX_ERR_BOOTLOADER;
    }
    return ALLTRAX_OK;
}

alltrax_error require_readable_firmware(alltrax_controller* ctrl)
{
    alltrax_error rc = require_program_mode(ctrl);
    if (rc || ctrl->firmware_read_override)
        return rc;
    if (!firmware_in_bounds(ctrl->controller_type, ctrl->boot_rev,
                            ctrl->program_rev)) {
        char boot[16], prgm[16];
        set_error_detail(ctrl,
            "Firmware boot %s / program %s is outside V0.001-V5.999; "
            "reads require the firmware read override",
            format_rev(ctrl->boot_rev, boot, sizeof(boot)),
            format_rev(ctrl->program_rev, prgm, sizeof(prgm)));
        return ALLTRAX_ERR_FIRMWARE;
    }
    return ALLTRAX_OK;
}

/* Writes require write-validated firmware. skip_fw_check widens this to
 * V0.001-V5.999; firmware outside those bounds is never written. The read
 * override has no effect on writes. */
alltrax_error require_writable_firmware(alltrax_controller* ctrl,
    bool skip_fw_check)
{
    alltrax_error rc = require_program_mode(ctrl);
    if (rc) return rc;
    char boot[16], prgm[16];
    format_rev(ctrl->boot_rev, boot, sizeof(boot));
    format_rev(ctrl->program_rev, prgm, sizeof(prgm));
    if (!firmware_in_bounds(ctrl->controller_type, ctrl->boot_rev,
                            ctrl->program_rev)) {
        set_error_detail(ctrl,
            "Firmware boot %s / program %s is outside V0.001-V5.999; "
            "writes are not allowed", boot, prgm);
        return ALLTRAX_ERR_FIRMWARE;
    }
    if (!skip_fw_check && !firmware_write_validated(ctrl->program_rev)) {
        set_error_detail(ctrl,
            "Firmware %s is not validated for writes (validated: V5.005); "
            "skip the firmware check to write firmware within V0.001-V5.999",
            prgm);
        return ALLTRAX_ERR_FIRMWARE;
    }
    return ALLTRAX_OK;
}

alltrax_error alltrax_set_firmware_read_override(alltrax_controller* ctrl,
    bool enabled)
{
    if (!ctrl)
        return ALLTRAX_ERR_INVALID_ARG;
    ctrl->firmware_read_override = enabled;
    return ALLTRAX_OK;
}

alltrax_error alltrax_get_info(alltrax_controller* ctrl, alltrax_info* out)
{
    alltrax_error family_rc = require_xct_controller(ctrl);
    if (family_rc) return family_rc;
    if (!out)
        return ALLTRAX_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->bootloader_mode = ctrl->bootloader_mode;
    alltrax_error rc;
    uint8_t buf[56];

    /* Controller identity (52 bytes) */
    rc = read_memory(ctrl, ADDR_CONTROLLER_INFO, 52, buf);
    if (rc) return rc;

    get_string(buf, 15, out->model, sizeof(out->model));
    out->controller_type = detect_controller_type(out->model);
    ctrl->controller_type = out->controller_type;
    ctrl->is_srx = strstr(out->model, "SRX") != NULL;
    rc = require_xct_controller(ctrl);
    if (rc) return rc;
    get_string(buf + 0x10, 15, out->build_date, sizeof(out->build_date));
    out->serial_number        = get_le32(buf + SERIAL_NUMBER_OFFSET);
    out->original_boot_rev    = get_le32(buf + 0x24);
    out->original_program_rev = get_le32(buf + 0x28);
    out->program_type         = get_le32(buf + 0x2C);
    out->hardware_rev         = get_le32(buf + 0x30);

    /* Hardware config (24 bytes) */
    rc = read_memory(ctrl, ADDR_HARDWARE_CONFIG, 24, buf);
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
    rc = read_memory(ctrl, ADDR_BOOT_REV, 4, boot_buf);
    if (rc) return rc;
    out->boot_rev = get_le32(boot_buf);

    /* Read program revision + version (6 bytes) */
    uint8_t prgm_buf[6];
    rc = read_memory(ctrl, ADDR_PRGM_REV, 6, prgm_buf);
    if (rc) return rc;
    out->program_rev = get_le32(prgm_buf);
    out->program_ver = get_le16(prgm_buf + 4);
    ctrl->boot_rev = out->boot_rev;
    ctrl->program_rev = out->program_rev;

    /* Format revision strings */
    format_rev(out->boot_rev, out->boot_rev_str,
               sizeof(out->boot_rev_str));
    format_rev(out->original_boot_rev, out->original_boot_rev_str,
               sizeof(out->original_boot_rev_str));
    format_rev(out->program_rev, out->program_rev_str,
               sizeof(out->program_rev_str));
    format_rev(out->original_program_rev, out->original_program_rev_str,
               sizeof(out->original_program_rev_str));

    /* Apply version gates to hardware config fields (XCT only).
     *
     * Old XCT firmware may not have reliable hardware config bytes,
     * so default to "all capabilities present" for backward compat. */
    if (out->controller_type == ALLTRAX_CONTROLLER_XCT) {
        /* Gate: HasBMSCan, HasThrotCan, HasUser2, HasUser3, HasAuxOut1,
         * HasAuxOut2.  If both OriginalBootRev and OriginalPrgmRev are 1
         * (V0.001), the hardware config bytes aren't programmed. */
        if (out->original_boot_rev == 1 &&
            out->original_program_rev == 1) {
            out->has_bms_can   = true;
            out->has_throt_can = true;
            out->has_user2     = true;
            out->has_user3     = true;
            out->has_aux_out1  = true;
            out->has_aux_out2  = true;
        }

        /* Gate: HasForward — trust hw config only if
         * OriginalPrgmRev >= 68 or PrgmVer == 200 */
        if (out->original_program_rev < 68 && out->program_ver != 200)
            out->has_forward = true;

        /* Gate: HasUser1 — threshold 70 */
        if (out->original_program_rev < 70 && out->program_ver != 200)
            out->has_user1 = true;

        /* Gate: CanHighSide, IsStockMode — OriginalPrgmRev >= 1008 */
        if (out->original_program_rev < 1008) {
            out->can_high_side = true;
            out->is_stock_mode = false;
        }

        /* Gate: ThrottlesAllowed — OriginalBootRev > 2 or
         * OriginalPrgmRev > 2. Older firmware allows every type except
         * EZGO (type 4, bit 3). */
        if (out->original_boot_rev <= 2 &&
            out->original_program_rev <= 2)
            out->throttles_allowed = 0x7F7;
    }

    /* Device family is already validated; firmware support is independent. */
    out->supported = firmware_in_bounds(out->controller_type,
                                        out->boot_rev, out->program_rev);
    out->write_validated = out->supported &&
                           firmware_write_validated(out->program_rev);

    return ALLTRAX_OK;
}

bool alltrax_has_feature(const alltrax_info* info, alltrax_feature feat)
{
    uint32_t orig_boot = info->original_boot_rev;
    uint32_t orig = info->original_program_rev;
    uint32_t prgm = info->program_rev;
    uint16_t ver = info->program_ver;

    switch (feat) {
    case ALLTRAX_FEAT_HW_CAPS:       return orig_boot != 1 || orig != 1;
    case ALLTRAX_FEAT_THROTTLE_CAPS: return orig_boot > 2 || orig > 2;
    case ALLTRAX_FEAT_FORWARD_INPUT: return orig >= 68 || ver == 200;
    case ALLTRAX_FEAT_USER1_INPUT:   return orig >= 70 || ver == 200;
    case ALLTRAX_FEAT_USER_PROFILES: return prgm >= 1005;
    case ALLTRAX_FEAT_USER_DEFAULTS: return orig >= 1007;
    case ALLTRAX_FEAT_CAN_HIGHSIDE:  return orig >= 1008;
    case ALLTRAX_FEAT_BAD_VARS_CODE: return prgm >= 1107;
    }
    return false;
}
