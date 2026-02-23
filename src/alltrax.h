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

alltrax_error alltrax_write_ram_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
    const alltrax_write_opts* opts);
alltrax_error alltrax_write_flash_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
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
