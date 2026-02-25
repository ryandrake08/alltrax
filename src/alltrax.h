/*
 * alltrax.h — Public API for liballtrax-usb
 *
 * Communicates with Alltrax motor controllers (XCT, SPM, SR series) via
 * USB HID. Reads/writes settings and monitors real-time status.
 *
 * Usage:
 *   alltrax_init();
 *
 *   alltrax_controller* ctrl;
 *   alltrax_error err = alltrax_open(&ctrl, false);
 *   if (err) { fprintf(stderr, "%s\n", alltrax_strerror(err)); return 1; }
 *
 *   alltrax_info info;
 *   err = alltrax_get_info(ctrl, &info);
 *
 *   alltrax_close(ctrl);
 *   alltrax_exit();
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
    ALLTRAX_ERR_PROTOCOL    =  -3,  /* Malformed response */
    ALLTRAX_ERR_DEVICE_FAIL =  -4,  /* Device reported failure */
    ALLTRAX_ERR_VERIFY      =  -5,  /* Write echo mismatch */
    ALLTRAX_ERR_FIRMWARE    =  -6,  /* Unknown firmware version */
    ALLTRAX_ERR_FLASH       =  -7,  /* FLASH operation failed */
    ALLTRAX_ERR_NOT_RUN     =  -8,  /* Not in RUN mode */
    ALLTRAX_ERR_INVALID_ARG =  -9,  /* Bad argument */
    ALLTRAX_ERR_ADDRESS     = -10,  /* Address out of range */
    ALLTRAX_ERR_NO_DEVICE   = -11,  /* No Alltrax device found */
    ALLTRAX_ERR_BLOCKED     = -12,  /* Write blocked (allow_writes=false) */
    ALLTRAX_ERR_NO_MEMORY   = -13,  /* Memory allocation failed */
} alltrax_error;

const char* alltrax_strerror(alltrax_error err);

/* ------------------------------------------------------------------ */
/* Opaque controller handle                                            */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_init(void);
void alltrax_exit(void);

typedef struct alltrax_controller alltrax_controller;

const char* alltrax_last_error_detail(const alltrax_controller* ctrl);

/* *out must be NULL on entry; returns ALLTRAX_ERR_INVALID_ARG otherwise. */
alltrax_error alltrax_open(alltrax_controller** out, bool allow_writes);
void alltrax_close(alltrax_controller* ctrl);

/* ------------------------------------------------------------------ */
/* Controller info                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Controller identity (0x08000800, 52 bytes) */
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

    /* Hardware config (0x08000880, 24 bytes) */
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
} alltrax_info;

alltrax_error alltrax_get_info(alltrax_controller* ctrl, alltrax_info* out);

typedef enum {
    ALLTRAX_FEAT_THROTTLE_CAPS,   /* V0.003+ (original_program_rev) */
    ALLTRAX_FEAT_FORWARD_INPUT,   /* V0.068+ (original_program_rev) */
    ALLTRAX_FEAT_USER1_INPUT,     /* V0.070+ (original_program_rev) */
    ALLTRAX_FEAT_USER_PROFILES,   /* V1.005+ (program_rev) */
    ALLTRAX_FEAT_USER_DEFAULTS,   /* V1.007+ (original_program_rev) */
    ALLTRAX_FEAT_CAN_HIGHSIDE,    /* V1.008+ (original_program_rev) */
    ALLTRAX_FEAT_BAD_VARS_CODE,   /* V1.107+ (program_rev) */
} alltrax_feature;

bool alltrax_has_feature(const alltrax_info* info, alltrax_feature feat);

/* ------------------------------------------------------------------ */
/* Variable system                                                     */
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

typedef struct {
    const char*      name;
    const char*      description;
    uint32_t         address;
    alltrax_var_type type;
    uint8_t          size;      /* string length; 0 for non-string */
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

const alltrax_var_def* alltrax_get_var_defs(alltrax_var_group group, size_t* count);
const alltrax_var_def* alltrax_find_var(const char* name);

/* Get Group name (e.g. "Normal_User"). Returns NULL if group is out of range. */
const char* alltrax_var_group_name(alltrax_var_group group);

/* Validate a display-unit value against the variable's bounds.
 * Returns ALLTRAX_OK if in range, ALLTRAX_ERR_INVALID_ARG if out of range.
 * Bools and strings always pass. */
alltrax_error alltrax_validate_var_value(const alltrax_var_def* var, double value);

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

alltrax_error alltrax_read_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out);

/* ------------------------------------------------------------------ */
/* Write operations                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    bool skip_cal;        /* Skip CAL/RUN bracket */
    bool skip_verify;     /* Skip read-back verification (FLASH only) */
    bool skip_goodset;    /* Skip GoodSet pre-check (FLASH only) */
    bool skip_fw_check;   /* Skip firmware version check */
} alltrax_write_opts;

alltrax_error alltrax_write_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
    const alltrax_write_opts* opts);

/* ------------------------------------------------------------------ */
/* Curve tables                                                        */
/* ------------------------------------------------------------------ */

#define ALLTRAX_CURVE_POINTS 16

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

const alltrax_curve_def* alltrax_find_curve(const char* name);
size_t alltrax_curve_count(void);
const alltrax_curve_def* alltrax_curve_by_index(size_t index);

alltrax_error alltrax_read_curve(alltrax_controller* ctrl,
    const alltrax_curve_def* def, alltrax_curve_data* out);
alltrax_error alltrax_read_curve_factory(alltrax_controller* ctrl,
    const alltrax_curve_def* def, alltrax_curve_data* out);
alltrax_error alltrax_write_curve(alltrax_controller* ctrl,
    const alltrax_curve_def* def, const alltrax_curve_data* data,
    const alltrax_write_opts* opts);

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

    int16_t bad_vars_code;

    int16_t speed_rpm;
    bool    speed_valid;

    int16_t user_input_state;
    bool    user1_input, user2_input, user3_input;
    int16_t user_profile_num;
    int16_t user1_input_percent, user2_input_percent;
} alltrax_monitor_data;

alltrax_error alltrax_read_monitor(alltrax_controller* ctrl,
    alltrax_monitor_data* out);

const char* alltrax_error_flag_name(int index);

/* ------------------------------------------------------------------ */
/* Special functions                                                   */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_reset_device(alltrax_controller* ctrl);

#ifdef __cplusplus
}
#endif

#endif /* ALLTRAX_H */
