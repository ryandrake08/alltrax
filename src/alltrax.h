/*
 * alltrax.h — Public API for liballtrax-usb
 *
 * Communicates with Alltrax XCT-family motor controllers via
 * USB HID. Reads/writes settings and monitors real-time status.
 *
 * API conventions:
 * - Names, definitions, and presets returned by lookup helpers, and strings
 *   returned by alltrax_strerror(), use static library-owned storage valid for
 *   the life of the program. Do not modify or free them.
 * - Device operations are synchronous. Callers own read output storage; use
 *   its contents only on ALLTRAX_OK, as failures may leave partial results.
 * - Write opts may be NULL to use zero-initialized defaults (all safeguards
 *   enabled). alltrax_reset_device() uses only skip_fw_check.
 *
 * Usage:
 *   alltrax_error err = alltrax_init();
 *   if (err) { fprintf(stderr, "%s\n", alltrax_strerror(err)); return 1; }
 *
 *   alltrax_controller* ctrl = NULL;  // alltrax_open() requires NULL
 *   err = alltrax_open(&ctrl, NULL, false);
 *   if (!err) {
 *       alltrax_info info;
 *       err = alltrax_get_info(ctrl, &info);
 *       if (!err)
 *           printf("Model: %s\n", info.model);
 *       alltrax_close(ctrl);
 *   }
 *   if (err)
 *       fprintf(stderr, "%s\n", alltrax_strerror(err));
 *
 *   alltrax_exit();
 *   return err ? 1 : 0;
 */

#ifndef ALLTRAX_H
#define ALLTRAX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    ALLTRAX_OK              =   0,
    ALLTRAX_ERR_USB         =  -1,  /* HID communication failure */
    ALLTRAX_ERR_TIMEOUT     =  -2,  /* No response */
    ALLTRAX_ERR_PROTOCOL    =  -3,  /* Malformed or incomplete response */
    ALLTRAX_ERR_DEVICE_FAIL =  -4,  /* Device reported failure */
    ALLTRAX_ERR_VERIFY      =  -5,  /* Write echo mismatch */
    ALLTRAX_ERR_FIRMWARE    =  -6,  /* Unknown firmware version */
    ALLTRAX_ERR_FLASH       =  -7,  /* FLASH operation failed */
    ALLTRAX_ERR_INVALID_ARG =  -8,  /* Bad argument */
    ALLTRAX_ERR_NO_DEVICE   =  -9,  /* No Alltrax device found */
    ALLTRAX_ERR_BLOCKED     = -10,  /* Write blocked (allow_writes=false) */
    ALLTRAX_ERR_NO_MEMORY   = -11,  /* Memory allocation failed */
    ALLTRAX_ERR_UNSUPPORTED = -12,  /* Unsupported USB identity or controller family */
    ALLTRAX_ERR_BOOTLOADER  = -13,  /* Settings and reset unavailable in bootloader mode */
} alltrax_error;

const char* alltrax_strerror(alltrax_error err);

/* ------------------------------------------------------------------ */
/* Controller types                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    ALLTRAX_CONTROLLER_UNKNOWN,
    ALLTRAX_CONTROLLER_XCT,
    ALLTRAX_CONTROLLER_SPM,
    ALLTRAX_CONTROLLER_SR,
    ALLTRAX_CONTROLLER_BMS,
    ALLTRAX_CONTROLLER_BMS2,
} alltrax_controller_type;

const char* alltrax_controller_type_name(alltrax_controller_type type);

/* ------------------------------------------------------------------ */
/* Opaque controller handle                                            */
/* ------------------------------------------------------------------ */

/* Initialize USB HID once before opening controllers.
 * Returns ALLTRAX_ERR_USB on failure. */
alltrax_error alltrax_init(void);
/* Release USB HID resources after closing all controller handles. */
void alltrax_exit(void);

typedef struct alltrax_controller alltrax_controller;

/* Get handle-owned diagnostics after a failed operation. Copy before another
 * operation or close; the text may be empty or stale if no detail was set.
 * Returns an empty string for NULL. Do not modify or free the returned text. */
const char* alltrax_last_error_detail(const alltrax_controller* ctrl);

/* out must be non-NULL and *out must be NULL on entry; returns
 * ALLTRAX_ERR_INVALID_ARG otherwise. On success the caller owns *out and must
 * release it with alltrax_close(). Given valid arguments, failure leaves *out NULL.
 * allow_writes=true permits writes/reset subject to safety checks; false
 * blocks them.
 * device_path: OS-level HID path (e.g. "/dev/hidraw0"), or NULL to
 * auto-detect. Only enumerated VID 0x23D4 / PID 0x0002 devices with an
 * XCT-family model prefix (XCT, SRX, NCT) are accepted. Opening reads the
 * model to validate identity; PID 1 and unrecognized families are rejected
 * with ALLTRAX_ERR_UNSUPPORTED. Unknown paths return ALLTRAX_ERR_NO_DEVICE.
 * Firmware overrides do not bypass device-family checks. */
alltrax_error alltrax_open(alltrax_controller** out,
    const char* device_path, bool allow_writes);
/* Close and free a controller handle. NULL is a no-op; a freed handle is invalid. */
void alltrax_close(alltrax_controller* ctrl);

/* Enumerate validated XCT-family devices using read-only USB requests.
 * If path is non-NULL, only that path is considered. Calls visit (if non-NULL)
 * once per responsive device and returns the count. Copy path and model if
 * needed after the callback returns. */
typedef void (*alltrax_device_visitor)(const char* path, const char* model,
    uint32_t serial, void* user);
size_t alltrax_list_devices(const char* path, alltrax_device_visitor visit,
    void* user);

/* alltrax_open() reads the boot and program revisions and StartUpSwitch.
 * Bootloader mode permits alltrax_get_info() and blocks other controller
 * operations with ALLTRAX_ERR_BOOTLOADER, regardless of firmware overrides.
 * Firmware policy in program mode:
 * - alltrax_get_info() works on any firmware.
 * - Other reads require XCT boot and program revisions within V0.001-V5.999
 *   and otherwise return ALLTRAX_ERR_FIRMWARE.
 * - Writes and reset require write-validated firmware (V5.005).
 *   alltrax_write_opts.skip_fw_check widens this to V0.001-V5.999.
 *   Firmware outside that range is never written.
 * Enabling the read override allows reads on any firmware; it does not
 * affect writes. Returns ALLTRAX_ERR_INVALID_ARG for a NULL controller. */
alltrax_error alltrax_set_firmware_read_override(alltrax_controller* ctrl,
    bool enabled);

/* ------------------------------------------------------------------ */
/* Controller info                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Controller type (detected from model prefix) */
    alltrax_controller_type controller_type;

    /* True if boot and program revisions are within V0.001-V5.999. */
    bool     supported;

    /* True if the program revision is validated for writes without
     * skip_fw_check. */
    bool     write_validated;

    /* Controller identity (52 bytes from INFOSET) */
    char     model[16];
    char     build_date[16];
    uint32_t serial_number;
    uint32_t original_boot_rev;
    uint32_t original_program_rev;
    uint32_t program_type;
    uint32_t hardware_rev;

    /* Boot revision (0x080067F0, 4 bytes) */
    uint32_t boot_rev;

    /* Program revision (0x0801FFF0, 6 bytes) */
    uint32_t program_rev;
    uint16_t program_ver;

    /* Pre-formatted revision strings (e.g. "V5.005") */
    char     boot_rev_str[16];
    char     original_boot_rev_str[16];
    char     program_rev_str[16];
    char     original_program_rev_str[16];

    /* Hardware config (24 bytes from INFOSET + 0x80) */
    uint16_t rated_voltage;
    uint16_t rated_amps;
    uint16_t rated_field_amps;
    bool     speed_sensor;
    bool     has_bms_can;
    bool     has_throt_can;
    bool     has_user2;
    bool     has_user3;
    bool     has_aux_out1;
    bool     has_aux_out2;
    uint32_t throttles_allowed;
    bool     has_forward;
    bool     has_user1;
    bool     can_high_side;
    bool     is_stock_mode;
    bool     bootloader_mode;     /* StartUpSwitch was nonzero at open */
} alltrax_info;

/* Read identity, hardware capabilities, and firmware into caller-owned out
 * (required, non-NULL). Works on any firmware of an accepted device family. */
alltrax_error alltrax_get_info(alltrax_controller* ctrl, alltrax_info* out);

typedef enum {
    ALLTRAX_FEAT_HW_CAPS,        /* orig_boot != 1 || orig_prgm != 1 */
    ALLTRAX_FEAT_THROTTLE_CAPS,  /* orig_boot > 2 || orig_prgm > 2 */
    ALLTRAX_FEAT_FORWARD_INPUT,  /* orig_prgm >= 68 || prgm_ver == 200 */
    ALLTRAX_FEAT_USER1_INPUT,    /* orig_prgm >= 70 || prgm_ver == 200 */
    ALLTRAX_FEAT_USER_PROFILES,  /* V1.005+ (program_rev) */
    ALLTRAX_FEAT_USER_DEFAULTS,  /* V1.007+ (original_program_rev) */
    ALLTRAX_FEAT_CAN_HIGHSIDE,   /* V1.008+ (original_program_rev) */
    ALLTRAX_FEAT_BAD_VARS_CODE,  /* V1.107+ (program_rev) */
} alltrax_feature;

bool alltrax_has_feature(const alltrax_info* info, alltrax_feature feat);

/* ------------------------------------------------------------------ */
/* Variable system                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    ALLTRAX_TYPE_BOOL,
    ALLTRAX_TYPE_UINT8,
    ALLTRAX_TYPE_INT8,
    ALLTRAX_TYPE_UINT16,
    ALLTRAX_TYPE_INT16,
    ALLTRAX_TYPE_UINT32,
    ALLTRAX_TYPE_INT32,
    ALLTRAX_TYPE_STRING,
} alltrax_var_type;

/* Sentinel values for unbounded variables */
#define ALLTRAX_NO_MIN INT64_MIN
#define ALLTRAX_NO_MAX INT64_MAX

/* Variable metadata. Device operations accept only the library's entries,
 * as returned by alltrax_find_var() and alltrax_get_var_defs(); any other
 * pointer, including a copy of an entry, returns ALLTRAX_ERR_INVALID_ARG
 * before device I/O. Entries stay valid for the life of the program. */
typedef struct {
    const char*      name;
    const char*      description;
    uint32_t         address;
    alltrax_var_type type;
    uint8_t          size;      /* string length 1-32; 0 for non-string */
    double           scale;     /* display = (raw - offset) * scale */
    int32_t          offset;    /* MachineOffset */
    const char*      unit;
    bool             writable;
    bool             is_flash;
    int64_t          raw_min;   /* ALLTRAX_NO_MIN = no minimum */
    int64_t          raw_max;   /* ALLTRAX_NO_MAX = no maximum */
} alltrax_var_def;

typedef enum {
    ALLTRAX_VARS_INFO,
    ALLTRAX_VARS_VOLTAGE,
    ALLTRAX_VARS_NORMAL_USER,
    ALLTRAX_VARS_USER1,
    ALLTRAX_VARS_USER2,
    ALLTRAX_VARS_TACH,
    ALLTRAX_VARS_OTHER_SETTINGS,
    ALLTRAX_VARS_THROTTLE,
    ALLTRAX_VARS_FIELD,
    ALLTRAX_VARS_FLAGS,
    ALLTRAX_VARS_RAW_ADC,
    ALLTRAX_VARS_AVG_ADC,
    ALLTRAX_VARS_READ_VALUES,
    ALLTRAX_VARS_WRITE_VALUES,
    ALLTRAX_VARS_GROUP_COUNT,
} alltrax_var_group;

/* Return the static group array and write its length to required, non-NULL
 * count. An invalid group returns NULL and sets *count to zero. */
const alltrax_var_def* alltrax_get_var_defs(alltrax_var_group group, size_t* count);
/* Find an exact, case-sensitive name (required, non-NULL); NULL if unknown. */
const alltrax_var_def* alltrax_find_var(const char* name);

/* Get Group name (e.g. "Normal_User"). Returns NULL if group is out of range. */
const char* alltrax_var_group_name(alltrax_var_group group);

/* Validate a library variable entry using the same checked conversion as encoding.
 * NULL and non-library entries, including copies, return ALLTRAX_ERR_INVALID_ARG.
 * Values must be finite.
 * Integer raw = round(value / scale) + offset must fit the type and explicit
 * bounds; scale must be finite and nonzero. Halfway values round away from zero.
 * Throttle_Type requires a defined integral enum value (0-11), without rounding.
 * Finite bools normalize zero to false and nonzero to true. Strings are rejected.
 * Returns ALLTRAX_OK if valid, ALLTRAX_ERR_INVALID_ARG otherwise.
 * Write permissions are enforced separately by alltrax_write_vars(). */
alltrax_error alltrax_validate_var_value(const alltrax_var_def* var, double value);

/* Validate text for a library string variable entry. NULL and non-library
 * entries, including copies, return ALLTRAX_ERR_INVALID_ARG.
 * Trailing spaces are ignored; the rest must be printable ASCII (0x20-0x7E)
 * and fit the field size. Values are
 * stored space-padded, and an empty string stores all spaces.
 * Returns ALLTRAX_OK if valid, ALLTRAX_ERR_INVALID_ARG otherwise. */
alltrax_error alltrax_validate_var_string(const alltrax_var_def* var,
    const char* text);

/* Get effective bounds in display units. Sets *min_out / *max_out to NAN
 * for bools, strings, or unbounded variables. */
void alltrax_var_display_bounds(const alltrax_var_def* var,
    double* min_out, double* max_out);

/* ------------------------------------------------------------------ */
/* Variable values                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const alltrax_var_def* def;
    union {
        uint8_t  u8;
        int8_t   i8;
        uint16_t u16;
        int16_t  i16;
        uint32_t u32;
        int32_t  i32;
        bool     b;
        char     str[33];
    } raw;
    double display;
} alltrax_var_value;

/* Extract raw value from a var_value as int64_t. Strings return 0. */
int64_t alltrax_var_raw_int64(const alltrax_var_value* val);

/* ------------------------------------------------------------------ */
/* Read operations                                                     */
/* ------------------------------------------------------------------ */

/* Read up to 256 library variables, coalescing nearby addresses. For nonzero
 * count, vars and out must be non-NULL; out needs room for count results,
 * returned in the same order as vars. The entire batch is checked before I/O;
 * a NULL or non-library entry returns
 * ALLTRAX_ERR_INVALID_ARG. */
alltrax_error alltrax_read_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out);

/* Read the factory values of user settings: like alltrax_read_vars(), but
 * each entry must lie in the user settings page and is read from the same
 * offset in the factory settings page. Other entries return
 * ALLTRAX_ERR_INVALID_ARG before I/O. */
alltrax_error alltrax_read_factory_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out);

/* ------------------------------------------------------------------ */
/* Write operations                                                    */
/* ------------------------------------------------------------------ */

/* Actions a write performed besides writing its batch. Each flag is set
 * once its action succeeded, so a failed write reports what already ran. */
typedef struct {
    bool factory_restored;  /* Factory settings copied to the user page */
    bool autocal_cleared;   /* Throttle auto-calibration cleared */
    bool date_stamped;      /* FieldMap_Date written */
    bool reset_sent;        /* Reset command sent; the controller reboots */
} alltrax_write_report;

/* Variable and curve writes attempt RUN restoration whenever CAL may have
 * been sent, including failed entry acknowledgements. If both the operation
 * and restoration fail, the original error is returned and error detail
 * contains both failures; a restoration-only failure returns its error. */
typedef struct {
    /* Debug and development bypasses */
    bool skip_cal;           /* Skip CAL/RUN bracket and its restoration */
    bool skip_verify;        /* Skip read-back verification (FLASH only) */
    /* User options */
    bool skip_fw_check;      /* Allow unvalidated firmware within V0.001-V5.999 */
    bool skip_post_write;    /* Debug bypass: skip automatic calibration clear, date stamp and reset */
    alltrax_write_report* report;  /* Optional; cleared, then filled by writes */
} alltrax_write_opts;

/* Write numeric/bool values in display units: alltrax_write_assignments()
 * with numeric items. Read-only definitions and string variables (which need
 * alltrax_write_assignments() text items) return ALLTRAX_ERR_INVALID_ARG
 * before device I/O, rejecting the entire batch, as do non-library entries.
 * Write options cannot bypass these checks or variable permissions. */
alltrax_error alltrax_write_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
    const alltrax_write_opts* opts);

/* ------------------------------------------------------------------ */
/* Curve tables                                                        */
/* ------------------------------------------------------------------ */

#define ALLTRAX_CURVE_POINTS 16

/* Curve metadata. As with variables, curve operations accept only the
 * library's entries (alltrax_find_curve(), alltrax_curve_by_index()). */
typedef struct {
    const char* name;           /* "linearization", "speed", etc. */
    const char* description;
    uint32_t    x_address;      /* user X array FLASH address */
    uint32_t    y_address;      /* user Y array FLASH address */
    uint32_t    factory_x_address;
    uint32_t    factory_y_address;
    double      x_scale;
    double      y_scale;
    const char* x_unit;
    const char* y_unit;
    int16_t     x_raw_max;      /* 4095 for throttle curves, 7500 for field */
    int16_t     y_raw_max;      /* 4095 for throttle curves, 5000 for field */
} alltrax_curve_def;

typedef struct {
    const alltrax_curve_def* def;
    double x[ALLTRAX_CURVE_POINTS];   /* display units */
    double y[ALLTRAX_CURVE_POINTS];   /* display units */
} alltrax_curve_data;

/* Find an exact, case-sensitive name (required, non-NULL); NULL if unknown. */
const alltrax_curve_def* alltrax_find_curve(const char* name);
size_t alltrax_curve_count(void);
/* Return the zero-based entry, or NULL if index >= alltrax_curve_count(). */
const alltrax_curve_def* alltrax_curve_by_index(size_t index);

/* Read user X/Y points in display units into required, non-NULL out.
 * def must be a library curve entry. */
alltrax_error alltrax_read_curve(alltrax_controller* ctrl,
    const alltrax_curve_def* def, alltrax_curve_data* out);
/* Read factory X/Y points in display units into required, non-NULL out.
 * def must be a library curve entry. */
alltrax_error alltrax_read_curve_factory(alltrax_controller* ctrl,
    const alltrax_curve_def* def, alltrax_curve_data* out);
/* Validate all 16 points without device I/O. `def` must be a library curve
 * entry (alltrax_find_curve() or alltrax_curve_by_index()); curve reads and
 * writes require the same. Points must be finite and round to raw
 * [0, axis_raw_max]. Zero padding and repeated points are permitted; no
 * ordering constraint is imposed. Returns ALLTRAX_ERR_INVALID_ARG otherwise. */
alltrax_error alltrax_validate_curve(const alltrax_curve_def* def,
    const alltrax_curve_data* data);
/* Write one user curve: alltrax_write_assignments() with a single curve item.
 * Enforces the same curve validation before device I/O, regardless of opts. */
alltrax_error alltrax_write_curve(alltrax_controller* ctrl,
    const alltrax_curve_def* def, const alltrax_curve_data* data,
    const alltrax_write_opts* opts);

/* ------------------------------------------------------------------ */
/* Batch writes                                                        */
/* ------------------------------------------------------------------ */

/* One item of a batch write. Set exactly one of `var` or `curve`.
 * - Numeric and bool variables use `number` (display units) and require
 *   text == NULL.
 * - String variables use `text` (see alltrax_validate_var_string()) and
 *   require it to be non-NULL.
 * - A user curve uses `curve` and `curve_data`. */
typedef struct {
    const alltrax_var_def* var;
    double number;
    const char* text;
    const alltrax_curve_def* curve;
    const alltrax_curve_data* curve_data;
} alltrax_assignment;

/* Write a batch of variables, strings, and user curves. Every item is
 * validated and encoded before device I/O; any invalid item, or two items
 * writing the same bytes, rejects the whole batch. Writes share one CAL/RUN
 * bracket if the controller starts in RUN. FLASH items share one page cycle.
 * With GoodSet checks enabled, invalid factory settings stop the batch.
 * Invalid user settings are restored from factory before applying the batch.
 * Limits: 16 RAM variables and 96 FLASH patches (each curve uses 2).
 * Permissions and firmware policy apply as for alltrax_write_vars(). Values
 * are checked against the catalog only: callers apply any limits based on
 * the controller's rating, hardware or other settings.
 * After successful writes and RUN cleanup, F_Table_Name stamps FieldMap_Date,
 * Throttle_Type or HPD clears throttle auto-calibration, and one reset is
 * sent after any settings-page write except a batch that writes only the
 * linearization curve and field-map data (Field variables, field and genfield
 * curves). A factory recovery always resets. Triggering depends on presence
 * in the batch, not on changed values. A post-write failure is reported after
 * settings were written. */
alltrax_error alltrax_write_assignments(alltrax_controller* ctrl,
    const alltrax_assignment* items, size_t count,
    const alltrax_write_opts* opts);

/* ------------------------------------------------------------------ */
/* Curve presets                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* name;        /* "linear", "0-5k-2wire", "standard", etc. */
    const char* description;
    const char* curve_type;  /* "linearization", "speed", or "torque" */
    double x[ALLTRAX_CURVE_POINTS];
    double y[ALLTRAX_CURVE_POINTS];
} alltrax_curve_preset;

size_t alltrax_curve_preset_count(void);
/* Return the zero-based preset, or NULL if index >= alltrax_curve_preset_count(). */
const alltrax_curve_preset* alltrax_curve_preset_by_index(size_t index);
/* Match both curve_type and name exactly (case-sensitive, both non-NULL).
 * Returns the static preset, or NULL if no pair matches. */
const alltrax_curve_preset* alltrax_find_curve_preset(
    const char* curve_type, const char* name);

/* ------------------------------------------------------------------ */
/* Field-map history                                                   */
/* ------------------------------------------------------------------ */

#define ALLTRAX_FIELD_MAP_HISTORY_MAX_RECORDS 56

typedef struct {
    uint16_t year;
    uint8_t month, day;
    char name[33];  /* NUL-terminated, with trailing padding removed */
} alltrax_field_map_history_record;

typedef struct {
    uint32_t map_count;    /* Total including records rolled off the page */
    size_t record_count;  /* Number of records still on the page */
    alltrax_field_map_history_record records[ALLTRAX_FIELD_MAP_HISTORY_MAX_RECORDS];
} alltrax_field_map_history;

/* Read field-map history on non-SRX XCT-family controllers with program
 * firmware newer than V2.002, subject to the normal firmware read bounds.
 * On failure, the required caller-owned output is unchanged. */
alltrax_error alltrax_read_field_map_history(alltrax_controller* ctrl,
    alltrax_field_map_history* out);

/* ------------------------------------------------------------------ */
/* Monitoring                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  errors[17];
    uint16_t error_history[17];

    bool    keyswitch, reverse, forward, footswitch;
    bool    fan_status, charger, blower, horn;
    bool    check_motor, hpd_ran;
    int16_t relay_on;

    double  battery_volts;
    double  output_amps;
    double  battery_amps;
    double  field_amps;

    double  temp_c;
    int16_t overtemp_cap;

    double  state_of_charge;

    int16_t throttle_local, throttle_position, throttle_pointer;

    uint16_t bad_vars_code;

    /* Meaningful only when alltrax_info.speed_sensor is true. */
    int16_t speed_rpm;

    bool    user1_input, user2_input, user3_input;

    /* Meaningful only when alltrax_has_feature(ALLTRAX_FEAT_USER_PROFILES)
     * and the UserInputs_State setting selects them: state 1 uses
     * user_profile_num (0 Normal, 1 User1, 2 User2) and user1_input_percent;
     * state 2 uses user1_input_percent and user2_input_percent. A negative
     * percent means the input is disconnected. */
    int16_t user_profile_num;
    int16_t user1_input_percent, user2_input_percent;
} alltrax_monitor_data;

/* Read one monitoring sample into required, non-NULL caller-owned out.
 * The sample spans several USB reads; the caller controls polling frequency. */
alltrax_error alltrax_read_monitor(alltrax_controller* ctrl,
    alltrax_monitor_data* out);

/* Name of error slot 0-16; returns NULL for any other index. */
const char* alltrax_error_flag_name(int index);
/* Name of throttle enum value 0-11; returns NULL for any other value. */
const char* alltrax_throttle_type_name(uint8_t type);

/* ------------------------------------------------------------------ */
/* Special functions                                                   */
/* ------------------------------------------------------------------ */

/* Reboot the controller. Follows the write firmware policy; opts may be NULL
 * and only skip_fw_check is used. Reads the model and serial number, then
 * sends the reset; ALLTRAX_OK confirms the command was sent. Use
 * alltrax_wait_for_reconnect() to wait for the reboot, then close the handle
 * and open a new one. */
alltrax_error alltrax_reset_device(alltrax_controller* ctrl,
    const alltrax_write_opts* opts);

/* Wait up to timeout_ms for the controller to come back after a reset sent
 * through this handle (by a reset, factory restore, auto-calibration clear,
 * or write). The reboot shows as the old handle failing, the old path
 * disappearing, or the controller not answering on its old path; then the
 * controller is matched by model and serial number using read-only
 * requests, on a new path or the old one. Returns ALLTRAX_OK when it answers,
 * ALLTRAX_ERR_TIMEOUT if it does not, and ALLTRAX_ERR_INVALID_ARG if no reset
 * was sent. The handle stays stale: close it and open a new one. */
alltrax_error alltrax_wait_for_reconnect(alltrax_controller* ctrl,
    unsigned timeout_ms);

/* Clear learned throttle calibration and send one reset. Uses the same
 * firmware/write gates as reset. No CAL bracket or wait for re-enumeration. */
alltrax_error alltrax_clear_autocal(alltrax_controller* ctrl,
    const alltrax_write_opts* opts);

/* Replace the user settings with the factory settings in one verified page
 * write, applying fixes for older controllers (original program revision
 * below V1.007). Afterwards, stamp FieldMap_Date, clear throttle
 * auto-calibration if the throttle type or HPD changes, and send one reset. Only skip_fw_check and report are used from opts,
 * which may be NULL. */
alltrax_error alltrax_restore_defaults(alltrax_controller* ctrl,
    const alltrax_write_opts* opts);

#ifdef __cplusplus
}
#endif

#endif /* ALLTRAX_H */
