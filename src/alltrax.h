/*
 * alltrax.h — Public API for liballtrax-usb
 *
 * Communicates with Alltrax motor controllers (XCT, SPM, SR series) via
 * USB HID. Reads/writes settings and monitors real-time status.
 *
 * Usage:
 *   alltrax_controller* ctrl;
 *   alltrax_error err = alltrax_open(&ctrl, false);
 *   if (err) { fprintf(stderr, "%s\n", alltrax_strerror(err)); return 1; }
 *
 *   alltrax_info info;
 *   err = alltrax_get_info(ctrl, &info);
 *
 *   alltrax_close(ctrl);
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
} alltrax_error;

const char* alltrax_strerror(alltrax_error err);

/* ------------------------------------------------------------------ */
/* Opaque controller handle                                            */
/* ------------------------------------------------------------------ */

typedef struct alltrax_controller alltrax_controller;

const char* alltrax_last_error_detail(const alltrax_controller* ctrl);

alltrax_error alltrax_open(alltrax_controller** out, bool allow_writes);
void alltrax_close(alltrax_controller* ctrl);

/* ------------------------------------------------------------------ */
/* Controller info                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char     model[16];
    char     build_date[16];
    uint32_t serial_number;
    uint32_t boot_rev;
    uint32_t original_boot_rev;
    uint32_t program_rev;
    uint32_t original_program_rev;
    uint16_t program_ver;
    uint16_t rated_voltage;
    uint16_t rated_amps;
} alltrax_info;

alltrax_error alltrax_get_info(alltrax_controller* ctrl, alltrax_info* out);
alltrax_error alltrax_check_firmware(alltrax_controller* ctrl);
char* alltrax_format_rev(uint32_t rev, char* buf, size_t buflen);

typedef enum {
    ALLTRAX_FEAT_USER_PROFILES,
    ALLTRAX_FEAT_USER_DEFAULTS,
    ALLTRAX_FEAT_CAN_HIGHSIDE,
    ALLTRAX_FEAT_FORWARD_INPUT,
    ALLTRAX_FEAT_USER1_INPUT,
    ALLTRAX_FEAT_THROTTLE_CAPS,
    ALLTRAX_FEAT_BAD_VARS_CODE,
} alltrax_feature;

bool alltrax_has_feature(const alltrax_controller* ctrl, alltrax_feature feat);

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
size_t alltrax_var_byte_size(const alltrax_var_def* var);

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

alltrax_error alltrax_decode_var(const uint8_t* data, size_t data_len,
    const alltrax_var_def* var, uint32_t base_address, alltrax_var_value* out);
int alltrax_encode_var(const alltrax_var_def* var, double value, uint8_t* buf);

/* ------------------------------------------------------------------ */
/* Read operations                                                     */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_read_memory(alltrax_controller* ctrl, uint32_t addr,
    size_t num_bytes, uint8_t* buf, size_t* out_len);
alltrax_error alltrax_read_memory_range(alltrax_controller* ctrl, uint32_t addr,
    size_t total_bytes, uint8_t* buf);
alltrax_error alltrax_read_var(alltrax_controller* ctrl,
    const alltrax_var_def* var, alltrax_var_value* out);
alltrax_error alltrax_read_var_group(alltrax_controller* ctrl,
    alltrax_var_group group, alltrax_var_value* out, size_t* count);

/* ------------------------------------------------------------------ */
/* Write operations                                                    */
/* ------------------------------------------------------------------ */

alltrax_error alltrax_write_memory(alltrax_controller* ctrl, uint32_t addr,
    const uint8_t* data, size_t len);
alltrax_error alltrax_write_ram_var(alltrax_controller* ctrl,
    const alltrax_var_def* var, double value);
alltrax_error alltrax_write_flash_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count);
alltrax_error alltrax_write_flash_var(alltrax_controller* ctrl,
    const alltrax_var_def* var, double value);

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

alltrax_error alltrax_page_erase(alltrax_controller* ctrl, uint8_t page);
alltrax_error alltrax_reset_device(alltrax_controller* ctrl);

#ifdef __cplusplus
}
#endif

#endif /* ALLTRAX_H */
