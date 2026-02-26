/*
 * variables.c — XCT variable definitions and encode/decode
 */

#include "internal.h"
#include <math.h>

/* Scale factors */
#define SCALE_VOLTS         0.1
#define SCALE_AMPS_PCT      (20.0 / 819.0)
#define SCALE_THROTTLE_RATE 0.5
#define SCALE_FIELD_X       0.1
#define SCALE_FIELD_Y       0.01
#define SCALE_TEMP          0.1289
#define OFFSET_TEMP         527
#define SCALE_ADC_V         0.0429
#define SCALE_ADC_BATV      0.04

/* ------------------------------------------------------------------ */
/* Variable group definitions                                          */
/* ------------------------------------------------------------------ */

/* Shorthand macros for defining variables */
#define VAR_RO(n, d, a, t, sc, off, u) { n, d, a, t, 0, sc, off, u, false, true, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }
#define VAR_RW(n, d, a, t, sc, off, u) { n, d, a, t, 0, sc, off, u, true, true, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }
#define VAR_RW_B(n, d, a, t, sc, off, u, mn, mx) { n, d, a, t, 0, sc, off, u, true, true, mn, mx }
#define VAR_BOOL_RO(n, d, a) { n, d, a, ALLTRAX_TYPE_BOOL, 0, 1.0, 0, "", false, true, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }
#define VAR_BOOL_RW(n, d, a) { n, d, a, ALLTRAX_TYPE_BOOL, 0, 1.0, 0, "", true, true, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }
#define VAR_STR_RO(n, d, a, sz) { n, d, a, ALLTRAX_TYPE_STRING, sz, 1.0, 0, "", false, true, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }
#define VAR_STR_RW(n, d, a, sz) { n, d, a, ALLTRAX_TYPE_STRING, sz, 1.0, 0, "", true, true, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }
/* RAM variables (not FLASH) */
#define VAR_RAM_RO(n, d, a, t, sc, off, u) { n, d, a, t, 0, sc, off, u, false, false, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }
#define VAR_RAM_BOOL_RO(n, d, a) { n, d, a, ALLTRAX_TYPE_BOOL, 0, 1.0, 0, "", false, false, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }
#define VAR_RAM_RW(n, d, a, t, sc, off, u) { n, d, a, t, 0, sc, off, u, true, false, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }
#define VAR_RAM_RW_B(n, d, a, t, sc, off, u, mn, mx) { n, d, a, t, 0, sc, off, u, true, false, mn, mx }
#define VAR_RAM_BOOL_RW(n, d, a) { n, d, a, ALLTRAX_TYPE_BOOL, 0, 1.0, 0, "", true, false, ALLTRAX_NO_MIN, ALLTRAX_NO_MAX }

/* --- VOLTAGE --- */
static const alltrax_var_def voltage_vars[] = {
    VAR_RW("AnalogInputs_Threshold", "Threshold for analog inputs ON", 0x08002002, ALLTRAX_TYPE_UINT16, SCALE_VOLTS, 0, "V"),
    VAR_RW("LoBat_Vlim", "Low battery voltage limit", 0x08002020, ALLTRAX_TYPE_UINT16, SCALE_VOLTS, 0, "V"),
    VAR_RW("HiBat_Vlim", "High battery voltage limit", 0x08002022, ALLTRAX_TYPE_UINT16, SCALE_VOLTS, 0, "V"),
    VAR_RW("BMS_Missing_HiBat_Vlim", "High battery V limit if BMS missing", 0x08002026, ALLTRAX_TYPE_UINT16, SCALE_VOLTS, 0, "V"),
};
#define VOLTAGE_COUNT (sizeof(voltage_vars) / sizeof(voltage_vars[0]))

/* --- NORMAL USER PROFILE --- */
static const alltrax_var_def normal_user_vars[] = {
    VAR_RW_B("N_Max_Batt_Motor_Amps", "Max battery amps (motoring)", 0x08002040, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", 0, 4095),
    VAR_RW_B("N_Max_Arm_Motor_Amps", "Max output amps (motoring)", 0x08002042, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", 0, 4095),
    VAR_RW_B("N_Max_Batt_Regen_Amps", "Max battery amps (regen)", 0x08002044, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("N_Max_Arm_Regen_Amps", "Max output amps (regen)", 0x08002046, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("N_RollDetect_BrakingCurrent", "Roll detect braking current", 0x08002048, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("N_Reverse_MotorS", "Max reverse speed", 0x0800204A, ALLTRAX_TYPE_INT16, 1.0, 0, "%", 0, 100),
    VAR_RW_B("N_Speed_Limit", "Speed limit", 0x0800204C, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM", 1000, 8000),
    VAR_BOOL_RW("N_Turbo", "Turbo mode", 0x0800204E),
    VAR_BOOL_RW("N_Hunting_Buggy", "Hunting Buggy Mode", 0x0800204F),
    VAR_RW_B("N_Max_Arm_Regen_Amps_Max", "Max regen amps (max range)", 0x08002050, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("N_Speed_Limit_Max", "Speed limit max range", 0x08002052, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM", -1, 8000),
    VAR_RW_B("N_Forward_Speed", "Max forward speed", 0x08002058, ALLTRAX_TYPE_INT16, 1.0, 0, "%", 0, 100),
    VAR_RW_B("N_Forward_Speed_Max", "Max forward speed max range", 0x0800205A, ALLTRAX_TYPE_INT16, 1.0, 0, "%", -1, 100),
    VAR_RW_B("N_Throttle_Rate", "Throttle accel rate", 0x080020A2, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU", 1, 32767),
    VAR_RW_B("N_Throttle_Rate_Max", "Throttle accel rate max range", 0x08002056, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU", -1, 32767),
};
#define NORMAL_USER_COUNT (sizeof(normal_user_vars) / sizeof(normal_user_vars[0]))

/* --- USER 1 PROFILE --- */
static const alltrax_var_def user1_vars[] = {
    VAR_RW_B("U1_Max_Batt_Motor_Amps", "Max battery amps (motoring)", 0x08002060, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", 0, 4095),
    VAR_RW_B("U1_Max_Arm_Motor_Amps", "Max output amps (motoring)", 0x08002062, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", 0, 4095),
    VAR_RW_B("U1_Max_Batt_Regen_Amps", "Max battery amps (regen)", 0x08002064, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("U1_Max_Arm_Regen_Amps", "Max output amps (regen)", 0x08002066, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("U1_RollDetect_BrakingCurrent", "Roll detect braking current", 0x08002068, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("U1_Reverse_MotorS", "Max reverse speed", 0x0800206A, ALLTRAX_TYPE_INT16, 1.0, 0, "%", 0, 100),
    VAR_RW_B("U1_Speed_Limit", "Speed limit", 0x0800206C, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM", 1000, 8000),
    VAR_BOOL_RW("U1_Turbo", "Turbo mode", 0x0800206E),
    VAR_BOOL_RW("U1_Hunting_Buggy", "Hunting Buggy Mode", 0x0800206F),
    VAR_RW_B("U1_Max_Arm_Regen_Amps_Max", "Max regen amps (max range)", 0x08002070, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("U1_Speed_Limit_Max", "Speed limit max range", 0x08002072, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM", -1, 8000),
    VAR_RW_B("U1_Forward_Speed", "Max forward speed", 0x08002078, ALLTRAX_TYPE_INT16, 1.0, 0, "%", 0, 100),
    VAR_RW_B("U1_Forward_Speed_Max", "Max forward speed max range", 0x0800207A, ALLTRAX_TYPE_INT16, 1.0, 0, "%", -1, 100),
    VAR_RW_B("U1_Throttle_Rate", "Throttle accel rate", 0x08002074, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU", 1, 32767),
    VAR_RW_B("U1_Throttle_Rate_Max", "Throttle accel rate max range", 0x08002076, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU", -1, 32767),
};
#define USER1_COUNT (sizeof(user1_vars) / sizeof(user1_vars[0]))

/* --- USER 2 PROFILE --- */
static const alltrax_var_def user2_vars[] = {
    VAR_RW_B("U2_Max_Batt_Motor_Amps", "Max battery amps (motoring)", 0x08002080, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", 0, 4095),
    VAR_RW_B("U2_Max_Arm_Motor_Amps", "Max output amps (motoring)", 0x08002082, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", 0, 4095),
    VAR_RW_B("U2_Max_Batt_Regen_Amps", "Max battery amps (regen)", 0x08002084, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("U2_Max_Arm_Regen_Amps", "Max output amps (regen)", 0x08002086, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("U2_RollDetect_BrakingCurrent", "Roll detect braking current", 0x08002088, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("U2_Reverse_MotorS", "Max reverse speed", 0x0800208A, ALLTRAX_TYPE_INT16, 1.0, 0, "%", 0, 100),
    VAR_RW_B("U2_Speed_Limit", "Speed limit", 0x0800208C, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM", 1000, 8000),
    VAR_BOOL_RW("U2_Turbo", "Turbo mode", 0x0800208E),
    VAR_BOOL_RW("U2_Hunting_Buggy", "Hunting Buggy Mode", 0x0800208F),
    VAR_RW_B("U2_Max_Arm_Regen_Amps_Max", "Max regen amps (max range)", 0x08002090, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%", -4095, 0),
    VAR_RW_B("U2_Speed_Limit_Max", "Speed limit max range", 0x08002092, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM", -1, 8000),
    VAR_RW_B("U2_Forward_Speed", "Max forward speed", 0x08002098, ALLTRAX_TYPE_INT16, 1.0, 0, "%", 0, 100),
    VAR_RW_B("U2_Forward_Speed_Max", "Max forward speed max range", 0x0800209A, ALLTRAX_TYPE_INT16, 1.0, 0, "%", -1, 100),
    VAR_RW_B("U2_Throttle_Rate", "Throttle accel rate", 0x08002094, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU", 1, 32767),
    VAR_RW_B("U2_Throttle_Rate_Max", "Throttle accel rate max range", 0x08002096, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU", -1, 32767),
};
#define USER2_COUNT (sizeof(user2_vars) / sizeof(user2_vars[0]))

/* --- THROTTLE --- */
static const alltrax_var_def throttle_vars[] = {
    VAR_RW("Throttle_Type", "Throttle type selector", 0x080020A0, ALLTRAX_TYPE_UINT8, 1.0, 0, ""),
    VAR_BOOL_RW("HPD", "High Pedal Disable", 0x080020A4),
    VAR_BOOL_RW("Relay_Off_At_Zero", "Turn off relay at zero throttle", 0x080020A6),
    VAR_BOOL_RW("Speed_Limit_On", "Speed limiting enabled", 0x080020A7),
    VAR_BOOL_RW("Tach_4_8", "false=4-pole, true=8-pole sensor", 0x080020A8),
    VAR_RW_B("ABS_Lo_Throt_Min", "ABS throttle low input min", 0x080020AA, ALLTRAX_TYPE_INT16, 1.0, 0, "", 0, 4095),
    VAR_RW_B("ABS_Lo_Throt_Max", "ABS throttle low input max", 0x080020AC, ALLTRAX_TYPE_INT16, 1.0, 0, "", 0, 4095),
    VAR_RW_B("ABS_Hi_Throt_Min", "ABS throttle high input min", 0x080020AE, ALLTRAX_TYPE_INT16, 1.0, 0, "", 0, 4095),
    VAR_RW_B("ABS_Hi_Throt_Max", "ABS throttle high input max", 0x080020B0, ALLTRAX_TYPE_INT16, 1.0, 0, "", 0, 4095),
    VAR_RW_B("ABS_Throt_Min", "ABS throttle min", 0x080020B2, ALLTRAX_TYPE_INT16, 1.0, 0, "", 0, 4095),
    VAR_RW_B("ABS_Throt_Max", "ABS throttle max", 0x080020B4, ALLTRAX_TYPE_INT16, 1.0, 0, "", 0, 4095),
    VAR_RW_B("ABS_HPD_Offset", "ABS throttle HPD offset", 0x080020B6, ALLTRAX_TYPE_INT16, 1.0, 0, "", 0, 4095),
    VAR_BOOL_RW("ABS_Slope", "ABS throttle slope", 0x080020B8),
    VAR_BOOL_RW("ABS_Differential", "false=single, true=differential", 0x080020B9),
};
#define THROTTLE_COUNT (sizeof(throttle_vars) / sizeof(throttle_vars[0]))

/* --- OTHER SETTINGS --- */
static const alltrax_var_def other_settings_vars[] = {
    VAR_BOOL_RW("High_Side_Output", "Changes output to low side", 0x080020BA),
    VAR_BOOL_RW("User3_Invert", "Charger interlock polarity", 0x080020BB),
    VAR_BOOL_RW("BMS_Expected", "True if BMS should be connected", 0x080020BC),
    VAR_RW_B("UserInputs_State", "0=Switches, 1=U1 Analog, 2=Both Analog", 0x080020BE, ALLTRAX_TYPE_INT16, 1.0, 0, "", 0, 2),
    VAR_STR_RW("Throttle_Type_Name", "Name of selected throttle type", 0x080020C0, 16),
};
#define OTHER_SETTINGS_COUNT (sizeof(other_settings_vars) / sizeof(other_settings_vars[0]))

/* --- TACH (factory defaults only — user versions in throttle_vars) --- */
static const alltrax_var_def tach_vars[] = {
    VAR_BOOL_RO("F_Speed_Limit_On", "Speed limiting enabled (factory)", 0x080010A7),
    VAR_BOOL_RO("F_Tach_4_8", "false=4-pole, true=8-pole (factory)", 0x080010A8),
};
#define TACH_COUNT (sizeof(tach_vars) / sizeof(tach_vars[0]))

/* --- FIELD (scalars + strings) --- */
static const alltrax_var_def field_vars[] = {
    VAR_STR_RW("F_Table_Name", "Field table name", 0x08002200, 32),
    VAR_RW_B("Reverse_Field_Weaken_Percent", "Reverse field weaken", 0x08002400, ALLTRAX_TYPE_UINT8, 1.0, 0, "%", 80, 100),
    VAR_RW_B("Zero_RPM_Field_Boost_Percent", "Zero RPM field boost", 0x08002401, ALLTRAX_TYPE_UINT8, 1.0, 0, "%", 1, 100),
    VAR_RW_B("RPM_Field_Boost_Stop", "RPM field boost stop", 0x08002402, ALLTRAX_TYPE_UINT16, 1.0, 0, "RPM", 50, 1000),
    VAR_RW_B("Max_Field_Weaken_Amps", "Max field weaken current", 0x08002404, ALLTRAX_TYPE_UINT16, 0.01, 0, "A", 0, 1000),
    VAR_RW_B("Turbo_Start_RPM", "Turbo start RPM", 0x08002406, ALLTRAX_TYPE_UINT16, 1.0, 0, "RPM", 1000, 8000),
    VAR_RW_B("Turbo_Weaken_Percent", "Turbo weaken percent", 0x08002408, ALLTRAX_TYPE_UINT8, 1.0, 0, "%", 0, 75),
    VAR_STR_RO("F_F_Table_Name", "Field table name (factory)", 0x08001200, 32),
    VAR_RO("F_Reverse_Field_Weaken_Percent", "Reverse field weaken (factory)", 0x08001400, ALLTRAX_TYPE_UINT8, 1.0, 0, "%"),
    VAR_RO("F_Zero_RPM_Field_Boost_Percent", "Zero RPM field boost (factory)", 0x08001401, ALLTRAX_TYPE_UINT8, 1.0, 0, "%"),
    VAR_RO("F_RPM_Field_Boost_Stop", "RPM field boost stop (factory)", 0x08001402, ALLTRAX_TYPE_UINT16, 1.0, 0, "RPM"),
    VAR_RO("F_Max_Field_Weaken_Amps", "Max field weaken current (factory)", 0x08001404, ALLTRAX_TYPE_UINT16, 0.01, 0, "A"),
    VAR_RO("F_Turbo_Start_RPM", "Turbo start RPM (factory)", 0x08001406, ALLTRAX_TYPE_UINT16, 1.0, 0, "RPM"),
    VAR_RO("F_Turbo_Weaken_Percent", "Turbo weaken percent (factory)", 0x08001408, ALLTRAX_TYPE_UINT8, 1.0, 0, "%"),
};
#define FIELD_COUNT (sizeof(field_vars) / sizeof(field_vars[0]))

/* --- RAW ADC (raw analog readings, read-only RAM) --- */
static const alltrax_var_def raw_adc_vars[] = {
    VAR_RAM_RO("Raw_Keyswitch", "Raw keyswitch ADC", 0x2000F0B0, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Raw_Reverse", "Raw reverse ADC", 0x2000F0B2, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Raw_BatV", "Raw battery voltage ADC", 0x2000F0B4, ALLTRAX_TYPE_INT16, SCALE_ADC_BATV, 0, "V"),
    VAR_RAM_RO("Raw_Temp", "Raw temperature ADC", 0x2000F0B6, ALLTRAX_TYPE_INT16, SCALE_TEMP, OFFSET_TEMP, "C"),
    VAR_RAM_RO("Raw_MotorHall", "Raw motor hall ADC", 0x2000F0BA, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Raw_F1Hall", "Raw field 1 hall ADC", 0x2000F0BC, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Raw_F2Hall", "Raw field 2 hall ADC", 0x2000F0BE, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Raw_ThrotHigh", "Raw throttle high ADC", 0x2000F0C0, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Raw_ThrotLow", "Raw throttle low ADC", 0x2000F0C2, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Raw_Footswitch", "Raw footswitch ADC", 0x2000F0C4, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Raw_Forward", "Raw forward ADC", 0x2000F0C6, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Raw_User1", "Raw user input 1 ADC", 0x2000F0C8, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Raw_User2", "Raw user input 2 ADC", 0x2000F0CA, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Raw_User3", "Raw user input 3 ADC", 0x2000F0CC, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
};
#define RAW_ADC_COUNT (sizeof(raw_adc_vars) / sizeof(raw_adc_vars[0]))

/* --- AVG ADC (averaged analog readings, read-only RAM) --- */
static const alltrax_var_def avg_adc_vars[] = {
    VAR_RAM_RO("Avg_Keyswitch", "Avg keyswitch ADC", 0x2000F0E0, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Avg_Reverse", "Avg reverse ADC", 0x2000F0E2, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Avg_BatV", "Avg battery voltage ADC", 0x2000F0E4, ALLTRAX_TYPE_INT16, SCALE_ADC_BATV, 0, "V"),
    VAR_RAM_RO("Avg_MotorHall", "Avg motor hall ADC", 0x2000F0EA, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Avg_F1Hall", "Avg field 1 hall ADC", 0x2000F0EC, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Avg_F2Hall", "Avg field 2 hall ADC", 0x2000F0EE, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Avg_ThrotHigh", "Avg throttle high ADC", 0x2000F0F0, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Avg_ThrotLow", "Avg throttle low ADC", 0x2000F0F2, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Avg_Footswitch", "Avg footswitch ADC", 0x2000F0F4, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Avg_Forward", "Avg forward ADC", 0x2000F0F6, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Avg_User1", "Avg user input 1 ADC", 0x2000F0F8, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Avg_User2", "Avg user input 2 ADC", 0x2000F0FA, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
    VAR_RAM_RO("Avg_User3", "Avg user input 3 ADC", 0x2000F0FC, ALLTRAX_TYPE_INT16, SCALE_ADC_V, 0, "V"),
};
#define AVG_ADC_COUNT (sizeof(avg_adc_vars) / sizeof(avg_adc_vars[0]))

/* --- FLAGS (error flags, read-only RAM) --- */
static const alltrax_var_def flag_vars[] = {
    VAR_RAM_BOOL_RO("Global_Shutdown", "Global shutdown", 0x2000F000),
    VAR_RAM_BOOL_RO("Hardware_Overcurrent", "Hardware overcurrent", 0x2000F001),
    VAR_RAM_BOOL_RO("OC_Retry_Running", "OC retry running", 0x2000F002),
    VAR_RAM_BOOL_RO("LOBAT", "Low battery", 0x2000F003),
    VAR_RAM_BOOL_RO("HIBAT", "High battery", 0x2000F004),
    VAR_RAM_BOOL_RO("Precharge_Fail", "Precharge fail", 0x2000F005),
    VAR_RAM_BOOL_RO("Overtemp", "Over temperature", 0x2000F006),
    VAR_RAM_BOOL_RO("Undertemp", "Under temperature", 0x2000F007),
    VAR_RAM_BOOL_RO("Range_Alarm", "Throttle range error", 0x2000F008),
    VAR_RAM_BOOL_RO("Hi_HThrot_Overrange", "Hi throttle overrange (high)", 0x2000F009),
    VAR_RAM_BOOL_RO("Lo_HThrot_Overrange", "Hi throttle overrange (low)", 0x2000F00A),
    VAR_RAM_BOOL_RO("Hi_LThrot_Overrange", "Lo throttle overrange (high)", 0x2000F00B),
    VAR_RAM_BOOL_RO("Lo_LThrot_Overrange", "Lo throttle overrange (low)", 0x2000F00C),
    VAR_RAM_BOOL_RO("Field_Open_Alarm", "Field open", 0x2000F00D),
    VAR_RAM_BOOL_RO("BMS_Missing", "BMS missing", 0x2000F00E),
    VAR_RAM_BOOL_RO("AuxStuck", "Aux stuck", 0x2000F00F),
    VAR_RAM_BOOL_RO("Bad_Vars", "Bad vars", 0x2000F010),
};
#define FLAGS_COUNT (sizeof(flag_vars) / sizeof(flag_vars[0]))

/* --- READ VALUES (monitoring, read-only RAM) --- */
static const alltrax_var_def read_values_vars[] = {
    VAR_RAM_BOOL_RO("Keyswitch_Input", "Key switch", 0x2000F090),
    VAR_RAM_BOOL_RO("Reverse_Input", "Reverse switch", 0x2000F091),
    VAR_RAM_RO("Relay_On", "Main contactor", 0x2000F092, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_BOOL_RO("Fan_Status", "Fan running", 0x2000F094),
    VAR_RAM_BOOL_RO("HPD_Ran", "HPD executed", 0x2000F095),
    VAR_RAM_RO("Bad_Vars_Code", "Settings error code", 0x2000F096, ALLTRAX_TYPE_UINT16, 1.0, 0, ""),
    VAR_RAM_BOOL_RO("Footswitch_Input", "Foot switch", 0x2000F098),
    VAR_RAM_BOOL_RO("Forward_Input", "Forward switch", 0x2000F099),
    VAR_RAM_BOOL_RO("User1_Input", "User input 1", 0x2000F09A),
    VAR_RAM_BOOL_RO("User2_Input", "User input 2", 0x2000F09B),
    VAR_RAM_BOOL_RO("User3_Input", "User input 3", 0x2000F09C),
    VAR_RAM_BOOL_RO("Charger_Plugged_In", "Charger detected", 0x2000F09D),
    VAR_RAM_BOOL_RO("Blower_Status", "Blower running", 0x2000F09E),
    VAR_RAM_BOOL_RO("Check_Motor_Status", "Motor check", 0x2000F09F),
    VAR_RAM_BOOL_RO("Horn_Status", "Horn active", 0x2000F0A0),
    VAR_RAM_RO("Avg_Temp", "Controller temperature", 0x2000F0E6, ALLTRAX_TYPE_INT16, SCALE_TEMP, OFFSET_TEMP, "C"),
    VAR_RAM_RO("BPlus_Volts", "Battery voltage", 0x2000F110, ALLTRAX_TYPE_INT16, SCALE_VOLTS, 0, "V"),
    VAR_RAM_RO("KSI_Volts", "Key switch voltage", 0x2000F112, ALLTRAX_TYPE_INT16, SCALE_VOLTS, 0, "V"),
    VAR_RAM_RO("Throttle_Local", "Throttle (local)", 0x2000F116, ALLTRAX_TYPE_INT16, 1.0, 0, "MU"),
    VAR_RAM_RO("Throttle_Position", "Throttle position", 0x2000F118, ALLTRAX_TYPE_INT16, 1.0, 0, "MU"),
    VAR_RAM_RO("Output_Amps", "Output current", 0x2000F11A, ALLTRAX_TYPE_INT32, 0.1, 0, "A"),
    VAR_RAM_RO("Field_Amps", "Field current", 0x2000F11E, ALLTRAX_TYPE_INT32, 0.01, 0, "A"),
    VAR_RAM_RO("Throttle_Pointer", "Throttle setpoint", 0x2000F128, ALLTRAX_TYPE_INT16, 1.0, 0, "MU"),
    VAR_RAM_RO("Overtemp_Cap", "Overtemp derating", 0x2000F138, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RAM_RO("Speed", "Motor speed", 0x2000F13A, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM"),
    VAR_RAM_RO("State_Of_Charge", "Battery SOC", 0x2000F14A, ALLTRAX_TYPE_INT16, 0.1, 0, "%"),
    VAR_RAM_RO("Battery_Amps", "Battery current", 0x2000F15A, ALLTRAX_TYPE_INT32, 0.1, 0, "A"),
};
#define READ_VALUES_COUNT (sizeof(read_values_vars) / sizeof(read_values_vars[0]))

/* --- WRITE VALUES (writable RAM controls) --- */
static const alltrax_var_def write_values_vars[] = {
    VAR_RAM_RW("RunMode", "0x00=RUN, 0xFF=CAL", 0x2000FFFA, ALLTRAX_TYPE_UINT8, 1.0, 0, ""),
    VAR_RAM_BOOL_RW("Fan_On", "Fan test trigger (momentary)", 0x2000F204),
};
#define WRITE_VALUES_COUNT (sizeof(write_values_vars) / sizeof(write_values_vars[0]))

/* --- INFO (controller identity, read-only FLASH) --- */
static const alltrax_var_def info_vars[] = {
    VAR_STR_RO("Model", "Controller model name", 0x08000800, 15),
    VAR_STR_RO("BuildDate", "Manufacturing date", 0x08000810, 15),
    VAR_RO("SerialNum", "Serial number", 0x08000820, ALLTRAX_TYPE_UINT32, 1.0, 0, ""),
    VAR_RO("OriginalBootRev", "Original bootloader version", 0x08000824, ALLTRAX_TYPE_UINT32, 1.0, 0, ""),
    VAR_RO("OriginalPrgmRev", "Original program version", 0x08000828, ALLTRAX_TYPE_UINT32, 1.0, 0, ""),
    VAR_RO("ProgramType", "Program type identifier", 0x0800082C, ALLTRAX_TYPE_UINT32, 1.0, 0, ""),
    VAR_RO("HardwareRev", "Hardware revision", 0x08000830, ALLTRAX_TYPE_UINT32, 1.0, 0, ""),
    VAR_RO("RatedVolts", "Rated voltage", 0x08000880, ALLTRAX_TYPE_UINT16, 1.0, 0, "V"),
    VAR_RO("RatedAmps", "Rated output current", 0x08000882, ALLTRAX_TYPE_UINT16, 1.0, 0, "A"),
    VAR_RO("BootRev", "Current bootloader version", 0x080067F0, ALLTRAX_TYPE_UINT32, 1.0, 0, ""),
    VAR_RO("PrgmRev", "Current program version", 0x0801FFF0, ALLTRAX_TYPE_UINT32, 1.0, 0, ""),
    VAR_RO("PrgmVer", "Program version number", 0x0801FFF4, ALLTRAX_TYPE_UINT16, 1.0, 0, ""),
    VAR_RO("Var_GoodSet", "User values valid marker", 0x08002000, ALLTRAX_TYPE_UINT16, 1.0, 0, ""),
    VAR_RO("Fact_GoodSet", "Factory values valid marker", 0x08001000, ALLTRAX_TYPE_UINT16, 1.0, 0, ""),
};
#define INFO_COUNT (sizeof(info_vars) / sizeof(info_vars[0]))

/* ------------------------------------------------------------------ */
/* Error flag names                                                    */
/* ------------------------------------------------------------------ */

static const char* error_flag_names[] = {
    "Global Shutdown",
    "Hardware Overcurrent",
    "OC Retry Running",
    "Low Battery",
    "High Battery",
    "Precharge Fail",
    "Over Temperature",
    "Under Temperature",
    "Throttle Range Error",
    "Hi Throttle Overrange (high)",
    "Hi Throttle Overrange (low)",
    "Lo Throttle Overrange (high)",
    "Lo Throttle Overrange (low)",
    "Field Open",
    "BMS Missing",
    "Aux Stuck",
    "Bad Vars",
};
#define ERROR_FLAG_COUNT 17

/* ------------------------------------------------------------------ */
/* Group name table                                                   */
/* ------------------------------------------------------------------ */

static const char* group_names[] = {
    [ALLTRAX_VARS_INFO]           = "Controller_Info",
    [ALLTRAX_VARS_VOLTAGE]        = "Voltage",
    [ALLTRAX_VARS_NORMAL_USER]    = "Normal_User",
    [ALLTRAX_VARS_USER1]          = "User_1",
    [ALLTRAX_VARS_USER2]          = "User_2",
    [ALLTRAX_VARS_TACH]           = "Tach",
    [ALLTRAX_VARS_OTHER_SETTINGS] = "Other_Settings",
    [ALLTRAX_VARS_THROTTLE]       = "Throttle",
    [ALLTRAX_VARS_FIELD]          = "Field",
    [ALLTRAX_VARS_FLAGS]          = "Flags",
    [ALLTRAX_VARS_RAW_ADC]        = "Raw_ADC",
    [ALLTRAX_VARS_AVG_ADC]        = "Avg_ADC",
    [ALLTRAX_VARS_READ_VALUES]    = "Read_Values",
    [ALLTRAX_VARS_WRITE_VALUES]   = "Write_Values",
};

/* ------------------------------------------------------------------ */
/* Group dispatch table                                                */
/* ------------------------------------------------------------------ */

struct var_group_entry {
    const alltrax_var_def* vars;
    size_t count;
};

static const struct var_group_entry group_table[] = {
    [ALLTRAX_VARS_INFO]           = { info_vars, INFO_COUNT },
    [ALLTRAX_VARS_VOLTAGE]        = { voltage_vars, VOLTAGE_COUNT },
    [ALLTRAX_VARS_NORMAL_USER]    = { normal_user_vars, NORMAL_USER_COUNT },
    [ALLTRAX_VARS_USER1]          = { user1_vars, USER1_COUNT },
    [ALLTRAX_VARS_USER2]          = { user2_vars, USER2_COUNT },
    [ALLTRAX_VARS_TACH]           = { tach_vars, TACH_COUNT },
    [ALLTRAX_VARS_OTHER_SETTINGS] = { other_settings_vars, OTHER_SETTINGS_COUNT },
    [ALLTRAX_VARS_THROTTLE]       = { throttle_vars, THROTTLE_COUNT },
    [ALLTRAX_VARS_FIELD]          = { field_vars, FIELD_COUNT },
    [ALLTRAX_VARS_FLAGS]          = { flag_vars, FLAGS_COUNT },
    [ALLTRAX_VARS_RAW_ADC]        = { raw_adc_vars, RAW_ADC_COUNT },
    [ALLTRAX_VARS_AVG_ADC]        = { avg_adc_vars, AVG_ADC_COUNT },
    [ALLTRAX_VARS_READ_VALUES]    = { read_values_vars, READ_VALUES_COUNT },
    [ALLTRAX_VARS_WRITE_VALUES]   = { write_values_vars, WRITE_VALUES_COUNT },
};
#define GROUP_TABLE_SIZE (sizeof(group_table) / sizeof(group_table[0]))

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

const char* alltrax_var_group_name(alltrax_var_group group)
{
    if ((size_t)group >= GROUP_TABLE_SIZE)
        return NULL;
    return group_names[group];
}

int64_t alltrax_var_raw_int64(const alltrax_var_value* val)
{
    switch (val->def->type) {
    case ALLTRAX_TYPE_BOOL:   return val->raw.b ? 1 : 0;
    case ALLTRAX_TYPE_UINT8:  return val->raw.u8;
    case ALLTRAX_TYPE_INT8:   return val->raw.i8;
    case ALLTRAX_TYPE_UINT16: return val->raw.u16;
    case ALLTRAX_TYPE_INT16:  return val->raw.i16;
    case ALLTRAX_TYPE_UINT32: return val->raw.u32;
    case ALLTRAX_TYPE_INT32:  return val->raw.i32;
    case ALLTRAX_TYPE_STRING: return 0;
    }
    return 0;
}

const alltrax_var_def* alltrax_get_var_defs(alltrax_var_group group, size_t* count)
{
    if ((size_t)group >= GROUP_TABLE_SIZE) {
        *count = 0;
        return NULL;
    }
    *count = group_table[group].count;
    return group_table[group].vars;
}

const alltrax_var_def* alltrax_find_var(const char* name)
{
    for (size_t g = 0; g < GROUP_TABLE_SIZE; g++) {
        const alltrax_var_def* vars = group_table[g].vars;
        size_t count = group_table[g].count;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(vars[i].name, name) == 0)
                return &vars[i];
        }
    }
    return NULL;
}

size_t alltrax_var_byte_size(const alltrax_var_def* var)
{
    switch (var->type) {
    case ALLTRAX_TYPE_BOOL:
    case ALLTRAX_TYPE_UINT8:
    case ALLTRAX_TYPE_INT8:
        return 1;
    case ALLTRAX_TYPE_UINT16:
    case ALLTRAX_TYPE_INT16:
        return 2;
    case ALLTRAX_TYPE_UINT32:
    case ALLTRAX_TYPE_INT32:
        return 4;
    case ALLTRAX_TYPE_STRING:
        return var->size;
    }
    return 0;
}

alltrax_error alltrax_decode_var(const uint8_t* data, size_t data_len,
    const alltrax_var_def* var, uint32_t base_address, alltrax_var_value* out)
{
    size_t offset = var->address - base_address;
    size_t var_size = alltrax_var_byte_size(var);

    if (offset + var_size > data_len)
        return ALLTRAX_ERR_INVALID_ARG;

    out->def = var;
    memset(&out->raw, 0, sizeof(out->raw));

    const uint8_t* p = data + offset;

    switch (var->type) {
    case ALLTRAX_TYPE_BOOL:
        out->raw.b = p[0] != 0;
        out->display = out->raw.b ? 1.0 : 0.0;
        return ALLTRAX_OK;

    case ALLTRAX_TYPE_UINT8:
        out->raw.u8 = p[0];
        out->display = ((double)out->raw.u8 - var->offset) * var->scale;
        return ALLTRAX_OK;

    case ALLTRAX_TYPE_INT8:
        out->raw.i8 = (int8_t)p[0];
        out->display = ((double)out->raw.i8 - var->offset) * var->scale;
        return ALLTRAX_OK;

    case ALLTRAX_TYPE_UINT16:
        out->raw.u16 = get_le16(p);
        out->display = ((double)out->raw.u16 - var->offset) * var->scale;
        return ALLTRAX_OK;

    case ALLTRAX_TYPE_INT16:
        out->raw.i16 = (int16_t)get_le16(p);
        out->display = ((double)out->raw.i16 - var->offset) * var->scale;
        return ALLTRAX_OK;

    case ALLTRAX_TYPE_UINT32:
        out->raw.u32 = get_le32(p);
        out->display = ((double)out->raw.u32 - var->offset) * var->scale;
        return ALLTRAX_OK;

    case ALLTRAX_TYPE_INT32:
        out->raw.i32 = (int32_t)get_le32(p);
        out->display = ((double)out->raw.i32 - var->offset) * var->scale;
        return ALLTRAX_OK;

    case ALLTRAX_TYPE_STRING:
        get_string(p, var->size, out->raw.str, sizeof(out->raw.str));
        out->display = 0.0;
        return ALLTRAX_OK;
    }

    return ALLTRAX_ERR_INVALID_ARG;
}

int alltrax_encode_var(const alltrax_var_def* var, double value, uint8_t* buf)
{
    /* Inverse of decode: raw = value / scale + offset */
    switch (var->type) {
    case ALLTRAX_TYPE_BOOL:
        buf[0] = value != 0.0 ? 1 : 0;
        return 1;

    case ALLTRAX_TYPE_UINT8: {
        int32_t raw = (int32_t)round(value / var->scale) + var->offset;
        buf[0] = (uint8_t)(raw & 0xFF);
        return 1;
    }

    case ALLTRAX_TYPE_INT8: {
        int32_t raw = (int32_t)round(value / var->scale) + var->offset;
        buf[0] = (uint8_t)(raw & 0xFF);
        return 1;
    }

    case ALLTRAX_TYPE_UINT16:
    case ALLTRAX_TYPE_INT16: {
        int32_t raw = (int32_t)round(value / var->scale) + var->offset;
        buf[0] = (uint8_t)(raw & 0xFF);
        buf[1] = (uint8_t)((raw >> 8) & 0xFF);
        return 2;
    }

    case ALLTRAX_TYPE_UINT32:
    case ALLTRAX_TYPE_INT32: {
        int64_t raw = (int64_t)round(value / var->scale) + var->offset;
        buf[0] = (uint8_t)(raw & 0xFF);
        buf[1] = (uint8_t)((raw >> 8) & 0xFF);
        buf[2] = (uint8_t)((raw >> 16) & 0xFF);
        buf[3] = (uint8_t)((raw >> 24) & 0xFF);
        return 4;
    }

    case ALLTRAX_TYPE_STRING:
        /* For strings, buf should already contain the string data.
         * This function doesn't handle string encoding — callers
         * pass string bytes directly via write operations. */
        return var->size;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Type range helpers                                                   */
/* ------------------------------------------------------------------ */

static void type_range(alltrax_var_type type, int64_t* lo, int64_t* hi)
{
    switch (type) {
    case ALLTRAX_TYPE_UINT8:  *lo = 0;      *hi = UINT8_MAX;  return;
    case ALLTRAX_TYPE_INT8:   *lo = INT8_MIN;  *hi = INT8_MAX;  return;
    case ALLTRAX_TYPE_UINT16: *lo = 0;      *hi = UINT16_MAX; return;
    case ALLTRAX_TYPE_INT16:  *lo = INT16_MIN; *hi = INT16_MAX; return;
    case ALLTRAX_TYPE_UINT32: *lo = 0;      *hi = UINT32_MAX; return;
    case ALLTRAX_TYPE_INT32:  *lo = INT32_MIN; *hi = INT32_MAX; return;
    default:                  *lo = INT64_MIN; *hi = INT64_MAX; return;
    }
}

/* Compute effective bounds = intersection of explicit bounds and type range */
static void effective_bounds(const alltrax_var_def* var, int64_t* lo, int64_t* hi)
{
    int64_t type_lo, type_hi;
    type_range(var->type, &type_lo, &type_hi);

    *lo = var->raw_min > type_lo ? var->raw_min : type_lo;
    *hi = var->raw_max < type_hi ? var->raw_max : type_hi;
}

alltrax_error alltrax_validate_var_value(const alltrax_var_def* var, double value)
{
    /* Bools and strings skip bounds checking */
    if (var->type == ALLTRAX_TYPE_BOOL || var->type == ALLTRAX_TYPE_STRING)
        return ALLTRAX_OK;

    /* Convert display value to raw */
    int64_t raw = (int64_t)round(value / var->scale) + var->offset;

    int64_t lo, hi;
    effective_bounds(var, &lo, &hi);

    if (raw < lo || raw > hi)
        return ALLTRAX_ERR_INVALID_ARG;

    return ALLTRAX_OK;
}

void alltrax_var_display_bounds(const alltrax_var_def* var,
    double* min_out, double* max_out)
{
    if (var->type == ALLTRAX_TYPE_BOOL || var->type == ALLTRAX_TYPE_STRING) {
        *min_out = NAN;
        *max_out = NAN;
        return;
    }

    int64_t lo, hi;
    effective_bounds(var, &lo, &hi);

    *min_out = ((double)lo - var->offset) * var->scale;
    *max_out = ((double)hi - var->offset) * var->scale;
}

const char* alltrax_error_flag_name(int index)
{
    if (index < 0 || index >= ERROR_FLAG_COUNT)
        return NULL;
    return error_flag_names[index];
}

/* ------------------------------------------------------------------ */
/* Curve table definitions                                             */
/* ------------------------------------------------------------------ */

static const alltrax_curve_def curve_defs[] = {
    {
        "linearization", "Throttle linearization curve",
        0x08002100, 0x08002120,     /* user X, Y */
        0x08001100, 0x08001120,     /* factory X, Y */
        SCALE_AMPS_PCT, SCALE_AMPS_PCT, "%", "%",
        4095, 4095,
    },
    {
        "speed", "Speed limit curve",
        0x08002140, 0x08002160,
        0x08001140, 0x08001160,
        SCALE_AMPS_PCT, SCALE_AMPS_PCT, "%", "%",
        4095, 4095,
    },
    {
        "torque", "Torque limit curve",
        0x08002180, 0x080021A0,
        0x08001180, 0x080011A0,
        SCALE_AMPS_PCT, SCALE_AMPS_PCT, "%", "%",
        4095, 4095,
    },
    {
        "field", "Field weakening curve",
        0x080021C0, 0x080021E0,
        0x080011C0, 0x080011E0,
        SCALE_FIELD_X, SCALE_FIELD_Y, "A", "A",
        7500, 5000,
    },
    {
        "genfield", "Generator field curve",
        0x08002220, 0x08002240,
        0x08001220, 0x08001240,
        SCALE_FIELD_X, SCALE_FIELD_Y, "A", "A",
        7500, 5000,
    },
};
#define CURVE_DEF_COUNT (sizeof(curve_defs) / sizeof(curve_defs[0]))

const alltrax_curve_def* alltrax_find_curve(const char* name)
{
    for (size_t i = 0; i < CURVE_DEF_COUNT; i++) {
        if (strcmp(curve_defs[i].name, name) == 0)
            return &curve_defs[i];
    }
    return NULL;
}

size_t alltrax_curve_count(void)
{
    return CURVE_DEF_COUNT;
}

const alltrax_curve_def* alltrax_curve_by_index(size_t index)
{
    if (index >= CURVE_DEF_COUNT)
        return NULL;
    return &curve_defs[index];
}

/* ------------------------------------------------------------------ */
/* Curve preset definitions                                           */
/* ------------------------------------------------------------------ */
/*   XCT_ThrotLin[12]  — linearization presets per throttle type      */
/*   Curves_Speed[1]    — standard speed preset                       */
/*   XCT_Curves_Torque[1] — XCT torque preset                         */
/* All values are in display units (percentages).                     */

static const alltrax_curve_preset curve_presets[] = {
    /* --- Linearization presets --- */
    {
        "linear", "Simple linear response",
        "linearization",
        { 0, 1, 99, 100, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0, 0, 100, 100, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
    },
    {
        "0-5k-2wire", "S-curve for 0-5K ohm 2-wire pot",
        "linearization",
        { 0, 1, 13, 24, 36, 50, 60, 70, 78, 85, 91, 99, 100, 0,0,0 },
        { 0, 0,  2,  4,  8, 15, 22, 32, 44, 58, 73, 100, 100, 0,0,0 },
    },
    {
        "5k-0-2wire", "Inverse S-curve for 5K-0 ohm 2-wire pot",
        "linearization",
        { 0, 1,  9, 15, 22, 30, 40, 50, 64, 76, 87, 99, 100, 0,0,0 },
        { 0, 0, 27, 42, 56, 68, 78, 85, 92, 96, 99, 100, 100, 0,0,0 },
    },
    {
        "yamaha", "Compressed range for Yamaha 0-1K throttle",
        "linearization",
        { 0, 1, 80, 100, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0, 0, 100, 100, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
    },
    {
        "clubcar", "Offset start for Club Car 3-wire throttle",
        "linearization",
        { 0, 5, 99, 100, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0, 0, 100, 100, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
    },

    /* --- Speed preset --- */
    {
        "standard", "Standard linear speed curve",
        "speed",
        { 0, 1, 99, 100, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0, 0, 100, 100, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
    },

    /* --- Torque preset (XCT-specific) --- */
    {
        "standard", "Aggressive XCT torque curve",
        "torque",
        { 0, 1,  7,  9, 12, 15, 20, 100, 0,0,0,0, 0,0,0,0 },
        { 0, 0, 69, 83, 93, 98, 100, 100, 0,0,0,0, 0,0,0,0 },
    },
};
#define CURVE_PRESET_COUNT (sizeof(curve_presets) / sizeof(curve_presets[0]))

size_t alltrax_curve_preset_count(void)
{
    return CURVE_PRESET_COUNT;
}

const alltrax_curve_preset* alltrax_curve_preset_by_index(size_t index)
{
    if (index >= CURVE_PRESET_COUNT)
        return NULL;
    return &curve_presets[index];
}

const alltrax_curve_preset* alltrax_find_curve_preset(
    const char* curve_type, const char* name)
{
    for (size_t i = 0; i < CURVE_PRESET_COUNT; i++) {
        if (strcmp(curve_presets[i].curve_type, curve_type) == 0 &&
            strcmp(curve_presets[i].name, name) == 0)
            return &curve_presets[i];
    }
    return NULL;
}
