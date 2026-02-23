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

/* ------------------------------------------------------------------ */
/* Variable group definitions                                          */
/* ------------------------------------------------------------------ */

/* Shorthand macros for defining variables */
#define VAR_RO(n, d, a, t, sc, off, u) { n, d, a, t, 0, sc, off, u, false, true }
#define VAR_RW(n, d, a, t, sc, off, u) { n, d, a, t, 0, sc, off, u, true, true }
#define VAR_BOOL_RO(n, d, a) { n, d, a, ALLTRAX_TYPE_BOOL, 0, 1.0, 0, "", false, true }
#define VAR_BOOL_RW(n, d, a) { n, d, a, ALLTRAX_TYPE_BOOL, 0, 1.0, 0, "", true, true }
#define VAR_STR_RO(n, d, a, sz) { n, d, a, ALLTRAX_TYPE_STRING, sz, 1.0, 0, "", false, true }
#define VAR_STR_RW(n, d, a, sz) { n, d, a, ALLTRAX_TYPE_STRING, sz, 1.0, 0, "", true, true }
/* RAM variables (not FLASH) */
#define VAR_RAM_RO(n, d, a, t, sc, off, u) { n, d, a, t, 0, sc, off, u, false, false }
#define VAR_RAM_BOOL_RO(n, d, a) { n, d, a, ALLTRAX_TYPE_BOOL, 0, 1.0, 0, "", false, false }
#define VAR_RAM_RW(n, d, a, t, sc, off, u) { n, d, a, t, 0, sc, off, u, true, false }
#define VAR_RAM_BOOL_RW(n, d, a) { n, d, a, ALLTRAX_TYPE_BOOL, 0, 1.0, 0, "", true, false }

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
    VAR_RW("N_Max_Batt_Motor_Amps", "Max battery amps (motoring)", 0x08002040, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("N_Max_Arm_Motor_Amps", "Max output amps (motoring)", 0x08002042, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("N_Max_Batt_Regen_Amps", "Max battery amps (regen)", 0x08002044, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("N_Max_Arm_Regen_Amps", "Max output amps (regen)", 0x08002046, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("N_RollDetect_BrakingCurrent", "Roll detect braking current", 0x08002048, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("N_Reverse_MotorS", "Max reverse speed", 0x0800204A, ALLTRAX_TYPE_INT16, 1.0, 0, "%"),
    VAR_RW("N_Speed_Limit", "Speed limit", 0x0800204C, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM"),
    VAR_BOOL_RW("N_Turbo", "Turbo mode", 0x0800204E),
    VAR_BOOL_RW("N_DriveStyle", "Drive style (true=Street)", 0x0800204F),
    VAR_RW("N_Max_Arm_Regen_Amps_Max", "Max regen amps (max range)", 0x08002050, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("N_Speed_Limit_Max", "Speed limit max range", 0x08002052, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM"),
    VAR_RW("N_Forward_Speed", "Max forward speed", 0x08002058, ALLTRAX_TYPE_INT16, 1.0, 0, "%"),
    VAR_RW("N_Forward_Speed_Max", "Max forward speed max range", 0x0800205A, ALLTRAX_TYPE_INT16, 1.0, 0, "%"),
    VAR_RW("N_Throttle_Rate", "Throttle accel rate", 0x080020A2, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU"),
    VAR_RW("N_Throttle_Rate_Max", "Throttle accel rate max range", 0x08002056, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU"),
};
#define NORMAL_USER_COUNT (sizeof(normal_user_vars) / sizeof(normal_user_vars[0]))

/* --- USER 1 PROFILE --- */
static const alltrax_var_def user1_vars[] = {
    VAR_RW("U1_Max_Batt_Motor_Amps", "Max battery amps (motoring)", 0x08002060, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U1_Max_Arm_Motor_Amps", "Max output amps (motoring)", 0x08002062, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U1_Max_Batt_Regen_Amps", "Max battery amps (regen)", 0x08002064, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U1_Max_Arm_Regen_Amps", "Max output amps (regen)", 0x08002066, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U1_RollDetect_BrakingCurrent", "Roll detect braking current", 0x08002068, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U1_Reverse_MotorS", "Max reverse speed", 0x0800206A, ALLTRAX_TYPE_INT16, 1.0, 0, "%"),
    VAR_RW("U1_Speed_Limit", "Speed limit", 0x0800206C, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM"),
    VAR_BOOL_RW("U1_Turbo", "Turbo mode", 0x0800206E),
    VAR_BOOL_RW("U1_DriveStyle", "Drive style (true=Street)", 0x0800206F),
    VAR_RW("U1_Max_Arm_Regen_Amps_Max", "Max regen amps (max range)", 0x08002070, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U1_Speed_Limit_Max", "Speed limit max range", 0x08002072, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM"),
    VAR_RW("U1_Forward_Speed", "Max forward speed", 0x08002078, ALLTRAX_TYPE_INT16, 1.0, 0, "%"),
    VAR_RW("U1_Forward_Speed_Max", "Max forward speed max range", 0x0800207A, ALLTRAX_TYPE_INT16, 1.0, 0, "%"),
    VAR_RW("U1_Throttle_Rate", "Throttle accel rate", 0x08002074, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU"),
    VAR_RW("U1_Throttle_Rate_Max", "Throttle accel rate max range", 0x08002076, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU"),
};
#define USER1_COUNT (sizeof(user1_vars) / sizeof(user1_vars[0]))

/* --- USER 2 PROFILE --- */
static const alltrax_var_def user2_vars[] = {
    VAR_RW("U2_Max_Batt_Motor_Amps", "Max battery amps (motoring)", 0x08002080, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U2_Max_Arm_Motor_Amps", "Max output amps (motoring)", 0x08002082, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U2_Max_Batt_Regen_Amps", "Max battery amps (regen)", 0x08002084, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U2_Max_Arm_Regen_Amps", "Max output amps (regen)", 0x08002086, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U2_RollDetect_BrakingCurrent", "Roll detect braking current", 0x08002088, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U2_Reverse_MotorS", "Max reverse speed", 0x0800208A, ALLTRAX_TYPE_INT16, 1.0, 0, "%"),
    VAR_RW("U2_Speed_Limit", "Speed limit", 0x0800208C, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM"),
    VAR_BOOL_RW("U2_Turbo", "Turbo mode", 0x0800208E),
    VAR_BOOL_RW("U2_DriveStyle", "Drive style (true=Street)", 0x0800208F),
    VAR_RW("U2_Max_Arm_Regen_Amps_Max", "Max regen amps (max range)", 0x08002090, ALLTRAX_TYPE_INT16, SCALE_AMPS_PCT, 0, "%"),
    VAR_RW("U2_Speed_Limit_Max", "Speed limit max range", 0x08002092, ALLTRAX_TYPE_INT16, 1.0, 0, "RPM"),
    VAR_RW("U2_Forward_Speed", "Max forward speed", 0x08002098, ALLTRAX_TYPE_INT16, 1.0, 0, "%"),
    VAR_RW("U2_Forward_Speed_Max", "Max forward speed max range", 0x0800209A, ALLTRAX_TYPE_INT16, 1.0, 0, "%"),
    VAR_RW("U2_Throttle_Rate", "Throttle accel rate", 0x08002094, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU"),
    VAR_RW("U2_Throttle_Rate_Max", "Throttle accel rate max range", 0x08002096, ALLTRAX_TYPE_INT16, SCALE_THROTTLE_RATE, 0, "MU"),
};
#define USER2_COUNT (sizeof(user2_vars) / sizeof(user2_vars[0]))

/* --- THROTTLE --- */
static const alltrax_var_def throttle_vars[] = {
    VAR_RW("Throttle_Type", "Throttle type selector", 0x080020A0, ALLTRAX_TYPE_UINT8, 1.0, 0, ""),
    VAR_BOOL_RW("HPD", "High Pedal Disable", 0x080020A4),
    VAR_BOOL_RW("Relay_Off_At_Zero", "Turn off relay at zero throttle", 0x080020A6),
    VAR_BOOL_RW("Speed_Limit_On", "Speed limiting enabled", 0x080020A7),
    VAR_BOOL_RW("Tach_4_8", "false=4-pole, true=8-pole sensor", 0x080020A8),
    VAR_RW("ABS_Lo_Throt_Min", "ABS throttle low input min", 0x080020AA, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RW("ABS_Lo_Throt_Max", "ABS throttle low input max", 0x080020AC, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RW("ABS_Hi_Throt_Min", "ABS throttle high input min", 0x080020AE, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RW("ABS_Hi_Throt_Max", "ABS throttle high input max", 0x080020B0, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RW("ABS_Throt_Min", "ABS throttle min", 0x080020B2, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RW("ABS_Throt_Max", "ABS throttle max", 0x080020B4, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_RW("ABS_HPD_Offset", "ABS throttle HPD offset", 0x080020B6, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_BOOL_RW("ABS_Slope", "ABS throttle slope", 0x080020B8),
    VAR_BOOL_RW("ABS_Differential", "false=single, true=differential", 0x080020B9),
};
#define THROTTLE_COUNT (sizeof(throttle_vars) / sizeof(throttle_vars[0]))

/* --- OTHER SETTINGS --- */
static const alltrax_var_def other_settings_vars[] = {
    VAR_BOOL_RW("High_Side_Output", "Changes output to low side", 0x080020BA),
    VAR_BOOL_RW("User3_Invert", "Charger interlock polarity", 0x080020BB),
    VAR_BOOL_RW("BMS_Expected", "True if BMS should be connected", 0x080020BC),
    VAR_RW("UserInputs_State", "0=Switches, 1=U1 Analog, 2=Both Analog", 0x080020BE, ALLTRAX_TYPE_INT16, 1.0, 0, ""),
    VAR_STR_RW("Throttle_Type_Name", "Name of selected throttle type", 0x080020C0, 16),
};
#define OTHER_SETTINGS_COUNT (sizeof(other_settings_vars) / sizeof(other_settings_vars[0]))

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
    [ALLTRAX_VARS_TACH]           = { NULL, 0 },
    [ALLTRAX_VARS_OTHER_SETTINGS] = { other_settings_vars, OTHER_SETTINGS_COUNT },
    [ALLTRAX_VARS_THROTTLE]       = { throttle_vars, THROTTLE_COUNT },
    [ALLTRAX_VARS_FIELD]          = { NULL, 0 },
    [ALLTRAX_VARS_FLAGS]          = { flag_vars, FLAGS_COUNT },
    [ALLTRAX_VARS_RAW_ADC]        = { NULL, 0 },
    [ALLTRAX_VARS_AVG_ADC]        = { NULL, 0 },
    [ALLTRAX_VARS_READ_VALUES]    = { read_values_vars, READ_VALUES_COUNT },
    [ALLTRAX_VARS_WRITE_VALUES]   = { write_values_vars, WRITE_VALUES_COUNT },
};
#define GROUP_TABLE_SIZE (sizeof(group_table) / sizeof(group_table[0]))

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

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

const char* alltrax_error_flag_name(int index)
{
    if (index < 0 || index >= ERROR_FLAG_COUNT)
        return NULL;
    return error_flag_names[index];
}
