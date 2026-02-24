# alltrax

Open source CLI tool and C library for communicating with
[Alltrax](https://www.alltraxinc.com/) motor controllers (XCT, SPM, SR series)
via USB HID.

```
$ alltrax info
Model:            XCT48400-DCS
Build Date:       04/04/2012
Serial Number:    123456
Boot Rev:         V5.002
Original Boot:    V5.002
Program Rev:      V5.005
Original Program: V5.005
Program Version:  0
Rated Voltage:    48V
Rated Current:    400A

$ alltrax get LoBat_Vlim HiBat_Vlim
LoBat_Vlim                     28.0 V
HiBat_Vlim                     62.0 V

$ alltrax monitor
============================================================
Alltrax Controller Monitor
============================================================

  No errors

--- Gauges ---
  Battery Voltage       51.2 V
  Output Current         0.0 A
  Battery Current        0.0 A
  Field Current         0.00 A
  Temperature           23.1 C
  State of Charge       85.0 %
  Motor Speed             0 RPM

--- Throttle ---
  Local                   0 MU
  Position                0 MU
  Setpoint (pointer)      0 MU

--- Digital Inputs ---
  Key Switch          ON
  Forward             off
  Reverse             off
  ...

  [Ctrl+C to stop]  Polling every 250ms
```

## Commands

| Command | Description |
|---------|-------------|
| `alltrax info` | Controller identity, firmware versions, ratings |
| `alltrax get [var...]` | Read settings (all or by name) |
| `alltrax write var=value [...]` | Write settings |
| `alltrax reset` | Reboot controller |
| `alltrax monitor` | Live polling display (Ctrl+C to stop) |
| `alltrax errors` | Active error flags and error history |

### Write examples

```sh
# Write a single FLASH setting
alltrax write LoBat_Vlim=28.1

# Write and reboot so firmware reloads from FLASH immediately
alltrax write --reset LoBat_Vlim=28.1

# Batch write — multiple settings in one FLASH page cycle
alltrax write LoBat_Vlim=28.1 HiBat_Vlim=61.0

# Write a RAM variable (takes effect immediately, not persistent)
alltrax write Fan_On=1
```

### Write flags

| Flag | Effect |
|------|--------|
| `--no-cal` | Skip CAL/RUN mode bracket (safe for bench programming with no motor) |
| `--no-verify` | Skip read-back verification after FLASH write |
| `--no-goodset` | Skip GoodSet corruption pre-check (required to write when a previous interrupted write left GoodSet invalid) |
| `--no-fw-version` | Skip firmware version check |
| `--reset` | Reboot controller after FLASH write |

## Building

### Dependencies

- C11 compiler (gcc or clang)
- GNU Make
- [hidapi](https://github.com/libusb/hidapi)

**Linux (Debian/Ubuntu):**

```sh
sudo apt install build-essential libhidapi-dev
```

**macOS (MacPorts):**

```sh
sudo port install hidapi
```

**macOS (Homebrew):**

```sh
brew install hidapi
```

### Compile

```sh
make            # builds build/alltrax, build/liballtrax-usb.a, build/test_alltrax
make test       # runs tests
make clean      # removes build/
```

## USB permissions

The controller appears as a HID device (`/dev/hidraw*`). By default this
requires root access. To allow your user account to access it, create a udev
rule:

```sh
# /etc/udev/rules.d/99-alltrax.rules
KERNEL=="hidraw*", ATTRS{idVendor}=="23d4", ATTRS{idProduct}=="0002", MODE="0666"
```

Then reload:

```sh
sudo udevadm control --reload-rules && sudo udevadm trigger
```

On macOS, no extra permissions are needed — HID devices are accessible to
regular users.

## Library API

The C library (`liballtrax-usb.a`) provides the full public API. The CLI is a
thin wrapper around it. Link with `-lalltrax-usb -lhidapi-hidraw -lm`.

### Quick start

```c
#include "alltrax.h"

alltrax_init();

alltrax_controller* ctrl = NULL;
alltrax_error err = alltrax_open(&ctrl, false);

alltrax_info info;
alltrax_get_info(ctrl, &info);
printf("Model: %s, Firmware: %s\n", info.model, info.program_rev_str);

alltrax_close(ctrl);
alltrax_exit();
```

### Lifecycle

```c
alltrax_error alltrax_init(void);
void          alltrax_exit(void);
```

Call `alltrax_init()` once before any other library call. Call `alltrax_exit()`
after all controllers are closed. These manage the underlying USB HID library.

```c
alltrax_error alltrax_open(alltrax_controller** out, bool allow_writes);
void          alltrax_close(alltrax_controller* ctrl);
```

`alltrax_open()` connects to the first Alltrax USB device found. `*out` must
be `NULL` on entry. Set `allow_writes` to `true` if you intend to write
settings; the library will reject write operations otherwise. Multiple
controllers can be open simultaneously.

### Error handling

All functions that can fail return `alltrax_error`:

| Code | Meaning |
|------|---------|
| `ALLTRAX_OK` | Success |
| `ALLTRAX_ERR_USB` | HID communication failure |
| `ALLTRAX_ERR_TIMEOUT` | No response from device |
| `ALLTRAX_ERR_PROTOCOL` | Malformed response |
| `ALLTRAX_ERR_DEVICE_FAIL` | Device reported failure |
| `ALLTRAX_ERR_VERIFY` | Write echo mismatch |
| `ALLTRAX_ERR_FIRMWARE` | Unsupported firmware version |
| `ALLTRAX_ERR_FLASH` | FLASH operation failed |
| `ALLTRAX_ERR_NOT_RUN` | Controller not in RUN mode |
| `ALLTRAX_ERR_INVALID_ARG` | Bad argument |
| `ALLTRAX_ERR_ADDRESS` | Address out of range |
| `ALLTRAX_ERR_NO_DEVICE` | No Alltrax device found |
| `ALLTRAX_ERR_BLOCKED` | Write blocked (`allow_writes=false`) |
| `ALLTRAX_ERR_NO_MEMORY` | Memory allocation failed |

```c
const char* alltrax_strerror(alltrax_error err);
const char* alltrax_last_error_detail(const alltrax_controller* ctrl);
```

`alltrax_strerror()` returns a short description for any error code.
`alltrax_last_error_detail()` returns a more specific message from the most
recent failed operation on a controller (e.g. the address that failed, the
expected vs actual value).

### Controller info

```c
alltrax_error alltrax_get_info(alltrax_controller* ctrl, alltrax_info* out);
```

Reads controller identity, firmware versions, ratings, and hardware
capabilities. Returns `ALLTRAX_ERR_FIRMWARE` if the firmware version is outside
the known supported range (V0.001-V5.999). The `alltrax_info` struct is still
fully populated even on firmware error, so callers can inspect it for
diagnostics.

**`alltrax_info` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `model` | `char[16]` | Model string (e.g. `"XCT48400-DCS"`) |
| `build_date` | `char[16]` | Factory build date |
| `serial_number` | `uint32_t` | Serial number |
| `boot_rev` | `uint32_t` | Boot revision (raw, e.g. `5002`) |
| `program_rev` | `uint32_t` | Program revision (raw, e.g. `5005`) |
| `program_ver` | `uint16_t` | Program version |
| `original_boot_rev` | `uint32_t` | Original boot revision at factory |
| `original_program_rev` | `uint32_t` | Original program revision at factory |
| `program_type` | `uint32_t` | Program type identifier |
| `hardware_rev` | `uint32_t` | Hardware revision |
| `boot_rev_str` | `char[16]` | Formatted boot revision (e.g. `"V5.002"`) |
| `program_rev_str` | `char[16]` | Formatted program revision (e.g. `"V5.005"`) |
| `original_boot_rev_str` | `char[16]` | Formatted original boot revision |
| `original_program_rev_str` | `char[16]` | Formatted original program revision |
| `rated_voltage` | `uint16_t` | Rated voltage (V) |
| `rated_amps` | `uint16_t` | Rated current (A) |
| `rated_field_amps` | `uint16_t` | Rated field current (A) |
| `speed_sensor` | `bool` | Has speed sensor |
| `has_bms_can` | `bool` | Has BMS CAN interface |
| `has_throt_can` | `bool` | Has throttle CAN interface |
| `has_forward` | `bool` | Has forward input |
| `has_user1` | `bool` | Has User 1 input |
| `has_user2` | `bool` | Has User 2 input |
| `has_user3` | `bool` | Has User 3 input |
| `has_aux_out1` | `bool` | Has auxiliary output 1 |
| `has_aux_out2` | `bool` | Has auxiliary output 2 |
| `can_high_side` | `bool` | Can do high-side drive |
| `is_stock_mode` | `bool` | Running in stock mode |
| `throttles_allowed` | `uint32_t` | Bitmask of allowed throttle types |

### Feature detection

```c
bool alltrax_has_feature(const alltrax_info* info, alltrax_feature feat);
```

Checks whether a firmware feature is available, based on the controller's
firmware version. Some features depend on `original_program_rev` (hardware
capabilities wired at the factory) and others on `program_rev` (current
firmware).

| Feature | Minimum version | Based on |
|---------|----------------|----------|
| `ALLTRAX_FEAT_THROTTLE_CAPS` | V0.003 | `original_program_rev` |
| `ALLTRAX_FEAT_FORWARD_INPUT` | V0.068 | `original_program_rev` |
| `ALLTRAX_FEAT_USER1_INPUT` | V0.070 | `original_program_rev` |
| `ALLTRAX_FEAT_USER_PROFILES` | V1.005 | `program_rev` |
| `ALLTRAX_FEAT_USER_DEFAULTS` | V1.007 | `original_program_rev` |
| `ALLTRAX_FEAT_CAN_HIGHSIDE` | V1.008 | `original_program_rev` |
| `ALLTRAX_FEAT_BAD_VARS_CODE` | V1.107 | `program_rev` |

### Variable system

Settings are accessed through named variables with type information and scaling.

```c
const alltrax_var_def* alltrax_find_var(const char* name);
const alltrax_var_def* alltrax_get_var_defs(alltrax_var_group group, size_t* count);
```

`alltrax_find_var()` looks up a variable by name (e.g. `"LoBat_Vlim"`),
returning `NULL` if not found. `alltrax_get_var_defs()` returns all variables
in a group.

**`alltrax_var_def` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `name` | `const char*` | Variable name (e.g. `"LoBat_Vlim"`) |
| `description` | `const char*` | Human-readable description |
| `address` | `uint32_t` | Memory address on controller |
| `type` | `alltrax_var_type` | Data type (see below) |
| `size` | `uint8_t` | String length (0 for non-string types) |
| `scale` | `double` | Display scaling: `display = (raw - offset) * scale` |
| `offset` | `int32_t` | Raw value offset before scaling |
| `unit` | `const char*` | Display unit (e.g. `"V"`, `"A"`, `"%"`) or `NULL` |
| `writable` | `bool` | Whether this variable can be written |
| `is_flash` | `bool` | `true` = FLASH (persistent), `false` = RAM |

**Variable types** (`alltrax_var_type`): `ALLTRAX_TYPE_BOOL`, `ALLTRAX_TYPE_UINT8`,
`ALLTRAX_TYPE_INT8`, `ALLTRAX_TYPE_UINT16`, `ALLTRAX_TYPE_INT16`,
`ALLTRAX_TYPE_UINT32`, `ALLTRAX_TYPE_INT32`, `ALLTRAX_TYPE_STRING`.

**Variable groups** (`alltrax_var_group`): `ALLTRAX_VARS_INFO`,
`ALLTRAX_VARS_VOLTAGE`, `ALLTRAX_VARS_NORMAL_USER`, `ALLTRAX_VARS_USER1`,
`ALLTRAX_VARS_USER2`, `ALLTRAX_VARS_TACH`, `ALLTRAX_VARS_OTHER_SETTINGS`,
`ALLTRAX_VARS_THROTTLE`, `ALLTRAX_VARS_FIELD`, `ALLTRAX_VARS_FLAGS`,
`ALLTRAX_VARS_RAW_ADC`, `ALLTRAX_VARS_AVG_ADC`, `ALLTRAX_VARS_READ_VALUES`,
`ALLTRAX_VARS_WRITE_VALUES`.

### Reading variables

```c
alltrax_error alltrax_read_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, size_t count, alltrax_var_value* out);
```

Reads one or more variables from the controller. `vars` is an array of
pointers to variable definitions, `count` is the number of variables, and `out`
is a caller-allocated array of `alltrax_var_value` structs to receive results.

**`alltrax_var_value` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `def` | `const alltrax_var_def*` | Pointer to the variable definition |
| `raw` | union | Raw value (`raw.u8`, `raw.i16`, `raw.u32`, `raw.b`, `raw.str`, etc.) |
| `display` | `double` | Scaled display value (e.g. raw 280 with scale 0.1 = 28.0) |

Example — read a single variable:

```c
const alltrax_var_def* var = alltrax_find_var("LoBat_Vlim");
alltrax_var_value val;
alltrax_read_vars(ctrl, &var, 1, &val);
printf("%s = %.1f %s\n", val.def->name, val.display, val.def->unit);
```

Example — read all variables in a group:

```c
size_t count;
const alltrax_var_def* defs = alltrax_get_var_defs(ALLTRAX_VARS_VOLTAGE, &count);

const alltrax_var_def* ptrs[64];
for (size_t i = 0; i < count; i++)
    ptrs[i] = &defs[i];

alltrax_var_value vals[64];
alltrax_read_vars(ctrl, ptrs, count, vals);
```

### Writing variables

```c
alltrax_error alltrax_write_ram_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
    const alltrax_write_opts* opts);
alltrax_error alltrax_write_flash_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
    const alltrax_write_opts* opts);
```

Both functions take arrays of variable definitions and display-scale values.
Values are automatically converted from display units to raw device format.
Pass `NULL` for `opts` to use safe defaults (all checks enabled). Pass an
`alltrax_write_opts` struct to control individual safety checks:

| Field | Effect |
|-------|--------|
| `skip_cal` | Skip CAL/RUN bracket (safe for bench programming with no motor) |
| `skip_verify` | Skip read-back verification (FLASH only) |
| `skip_goodset` | Skip GoodSet corruption pre-check (FLASH only) |
| `skip_fw_check` | Skip firmware version check (FLASH only) |

**RAM writes** take effect immediately but are not persistent across power
cycles. The library enters CAL mode, writes all variables, then restores RUN
mode. Honors `skip_cal`.

**FLASH writes** are persistent. The library performs a full page cycle: read
the 2KB settings page, patch in the new values, erase the page, write it back,
and verify. Multiple variables are batched into a single page cycle. The
controller requires `allow_writes=true` and firmware V5.005. Only variables
with `is_flash=true` can be written with this function. Honors all four flags.

Example:

```c
const alltrax_var_def* vars[] = {
    alltrax_find_var("LoBat_Vlim"),
    alltrax_find_var("HiBat_Vlim"),
};
double values[] = { 28.1, 61.0 };
alltrax_write_flash_vars(ctrl, vars, values, 2, NULL);
```

### Monitoring

```c
alltrax_error alltrax_read_monitor(alltrax_controller* ctrl,
    alltrax_monitor_data* out);
```

Reads real-time status in a single call (6 USB reads internally).

**`alltrax_monitor_data` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `errors[17]` | `uint8_t` | Active error flags (one per error type) |
| `error_history[17]` | `uint16_t` | Lifetime error counts |
| `battery_volts` | `double` | Battery voltage (V) |
| `output_amps` | `double` | Output current (A) |
| `battery_amps` | `double` | Battery current (A) |
| `field_amps` | `double` | Field current (A) |
| `temp_c` | `double` | Controller temperature (C) |
| `state_of_charge` | `double` | Battery state of charge (%) |
| `speed_rpm` | `int16_t` | Motor speed (RPM) |
| `throttle_local` | `int16_t` | Throttle input (machine units) |
| `throttle_position` | `int16_t` | Throttle position (machine units) |
| `throttle_pointer` | `int16_t` | Throttle setpoint (machine units) |
| `overtemp_cap` | `int16_t` | Over-temperature current cap |
| `keyswitch` | `bool` | Key switch state |
| `forward` | `bool` | Forward input |
| `reverse` | `bool` | Reverse input |
| `footswitch` | `bool` | Footswitch input |
| `fan_status` | `bool` | Fan running |
| `charger` | `bool` | Charger plugged in |
| `blower` | `bool` | Blower running |
| `horn` | `bool` | Horn active |
| `check_motor` | `bool` | Check motor warning |
| `hpd_ran` | `bool` | HPD (high pedal disable) triggered |
| `relay_on` | `int16_t` | Main contactor relay state |
| `bad_vars_code` | `int16_t` | Settings corruption code |
| `user1_input` | `bool` | User 1 input state |
| `user2_input` | `bool` | User 2 input state |
| `user3_input` | `bool` | User 3 input state |
| `user_input_state` | `int16_t` | Combined user input state |
| `user_profile_num` | `int16_t` | Active user profile number |
| `user1_input_percent` | `int16_t` | User 1 input level (%) |
| `user2_input_percent` | `int16_t` | User 2 input level (%) |

```c
const char* alltrax_error_flag_name(int index);
```

Returns the name of error flag at `index` (0-16), e.g. `"Overcurrent"`,
`"Overtemperature"`. Returns `NULL` for invalid indices.

### Device reset

```c
alltrax_error alltrax_reset_device(alltrax_controller* ctrl);
```

Sends a reset command. The controller reboots immediately (USB disconnects).
The controller handle becomes invalid after this call — close it without
expecting further communication. Used after FLASH writes to make new settings
take effect without a power cycle.

## Variable Reference

All controller settings and status values are accessed through named variables.
Variables are organized into groups and stored in either **FLASH** (persistent
across power cycles) or **RAM** (live state, lost on power cycle).

Factory defaults vary by controller model and electrical ratings — they are
programmed at the factory. Read your controller's factory defaults via the
`F_`-prefixed read-only copies (e.g. `F_Reverse_Field_Weaken_Percent`).

### FLASH Variables (Persistent Settings)

FLASH variables are stored in the STM32's FLASH memory and persist across power
cycles. Writing a FLASH variable requires a full page cycle (read 2KB page,
patch values, erase, rewrite, verify). Multiple FLASH writes in one
`alltrax write` command are batched into a single page cycle.

#### Info (read-only)

Controller identity and hardware capabilities. Programmed at the factory.

| Variable | Description | Unit |
|----------|-------------|------|
| `Model` | Controller model name (e.g. "XCT48400-DCS") | |
| `BuildDate` | Manufacturing date | |
| `SerialNum` | Serial number | |
| `OriginalBootRev` | Original bootloader revision | |
| `OriginalPrgmRev` | Original program revision | |
| `ProgramType` | Program type identifier | |
| `HardwareRev` | Hardware revision | |
| `RatedVolts` | Rated voltage | V |
| `RatedAmps` | Rated output current | A |
| `BootRev` | Current bootloader revision | |
| `PrgmRev` | Current program revision | |
| `PrgmVer` | Program version number | |
| `Var_GoodSet` | User settings valid marker (0x0000 = valid) | |
| `Fact_GoodSet` | Factory settings valid marker (0x0000 = valid) | |

#### Voltage

Battery voltage limits and analog input threshold.

| Variable | Description | Unit |
|----------|-------------|------|
| `AnalogInputs_Threshold` | Voltage above which analog inputs read as ON | V |
| `LoBat_Vlim` | Low battery voltage cutoff | V |
| `HiBat_Vlim` | High battery voltage cutoff | V |
| `BMS_Missing_HiBat_Vlim` | High battery voltage cutoff when BMS is missing | V |

#### User Profiles (Normal, User 1, User 2)

Three user profiles with identical structure. The **Normal** profile uses the
`N_` prefix, **User 1** uses `U1_`, and **User 2** uses `U2_`. The active
profile is selected by the User input switches (requires firmware V1.005+).

Amp values are percentages of the controller's rated current. For a 400A
controller, 100% = 400A. The `_Max` suffix variables define the upper limit of
the user-adjustable range in the Toolkit UI; a value of **-1 means disabled**
(no user-adjustable range).

The table below shows the Normal profile (`N_` prefix). User 1 (`U1_`) and
User 2 (`U2_`) have the same variables at different addresses.

| Variable | Description | Unit | Bounds |
|----------|-------------|------|--------|
| `N_Max_Batt_Motor_Amps` | Max battery current (motoring) | % | 0 – 100 |
| `N_Max_Arm_Motor_Amps` | Max output current (motoring) | % | 0 – 100 |
| `N_Max_Batt_Regen_Amps` | Max battery current (regen, negative) | % | -100 – 0 |
| `N_Max_Arm_Regen_Amps` | Max output current (regen, negative) | % | -100 – 0 |
| `N_RollDetect_BrakingCurrent` | Roll detect braking current (negative) | % | -100 – 0 |
| `N_Reverse_MotorS` | Max reverse speed | % | 0 – 100 |
| `N_Speed_Limit` | Speed limit (when `Speed_Limit_On` is enabled) | RPM | 1000 – 8000 |
| `N_Turbo` | Turbo mode (field weakening at high RPM) | bool | |
| `N_DriveStyle` | Drive style: No = Turf (gentle), Yes = Street (full power) | bool | |
| `N_Max_Arm_Regen_Amps_Max` | Max regen amps user-adjustable ceiling | % | -100 – 0 |
| `N_Speed_Limit_Max` | Speed limit user-adjustable ceiling (-1 = disabled) | RPM | -1 – 8000 |
| `N_Forward_Speed` | Max forward speed (vehicles without speed sensor) | % | 0 – 100 |
| `N_Forward_Speed_Max` | Forward speed user-adjustable ceiling (-1 = disabled) | % | -1 – 100 |
| `N_Throttle_Rate` | Throttle acceleration rate | MU | 0.5 – 16383.5 |
| `N_Throttle_Rate_Max` | Throttle rate user-adjustable ceiling (-1 = disabled) | MU | -0.5 – 16383.5 |

#### Throttle

Throttle type selection, calibration values, and related settings.

`Throttle_Type` selects the throttle hardware:

| Value | Type |
|-------|------|
| 0 | None |
| 1 | 0-5k ohm (2-wire) |
| 2 | 5k-0 ohm (2-wire, reversed) |
| 3 | 0-5V |
| 4 | EZGO ITS |
| 5 | Yamaha (0-1k ohm) |
| 6 | Taylor-Dunn |
| 7 | Club Car (5k, 3-wire) |
| 8 | Digital |
| 9 | Pump |
| 10 | USB |
| 11 | Absolute |

| Variable | Description | Unit | Bounds |
|----------|-------------|------|--------|
| `Throttle_Type` | Throttle hardware type (see table above) | | |
| `HPD` | High Pedal Disable — block drive if throttle is pressed at key-on | bool | |
| `Relay_Off_At_Zero` | Turn off main contactor at zero throttle input | bool | |
| `Speed_Limit_On` | Enable speed limiting (uses `N_Speed_Limit` etc.) | bool | |
| `Tach_4_8` | Speed sensor type: No = 4-pole, Yes = 8-pole | bool | |
| `ABS_Lo_Throt_Min` | Absolute throttle calibration: low input minimum | | 0 – 4095 |
| `ABS_Lo_Throt_Max` | Absolute throttle calibration: low input maximum | | 0 – 4095 |
| `ABS_Hi_Throt_Min` | Absolute throttle calibration: high input minimum | | 0 – 4095 |
| `ABS_Hi_Throt_Max` | Absolute throttle calibration: high input maximum | | 0 – 4095 |
| `ABS_Throt_Min` | Absolute throttle calibration: output minimum | | 0 – 4095 |
| `ABS_Throt_Max` | Absolute throttle calibration: output maximum | | 0 – 4095 |
| `ABS_HPD_Offset` | Absolute throttle calibration: HPD offset | | 0 – 4095 |
| `ABS_Slope` | Absolute throttle slope direction | bool | |
| `ABS_Differential` | Absolute throttle: No = single input, Yes = differential | bool | |

The `ABS_*` calibration values are only used when `Throttle_Type` is set to
Absolute (11). For other throttle types, the firmware uses built-in calibration
curves.

#### Other Settings

| Variable | Description | Unit | Bounds |
|----------|-------------|------|--------|
| `High_Side_Output` | Output drive mode (controller-specific) | bool | |
| `User3_Invert` | Charger interlock input polarity | bool | |
| `BMS_Expected` | Whether a Battery Management System should be connected | bool | |
| `UserInputs_State` | User input mode: 0 = switches, 1 = U1 analog, 2 = both analog | | 0 – 2 |
| `Throttle_Type_Name` | Custom display name for the selected throttle (16 chars) | | |

#### Tach (read-only factory defaults)

Factory copies of speed sensor settings, for reference.

| Variable | Description | Unit |
|----------|-------------|------|
| `F_Speed_Limit_On` | Speed limiting enabled (factory) | bool |
| `F_Tach_4_8` | Speed sensor type (factory) | bool |

#### Field

Field weakening and turbo parameters control how the controller drives the motor
field winding at higher speeds to trade torque for RPM. The writable variables
are in the user settings page; the `F_`-prefixed versions are read-only factory
defaults.

| Variable | Description | Unit | Bounds |
|----------|-------------|------|--------|
| `F_Table_Name` | Field table name (32 chars) | | |
| `Reverse_Field_Weaken_Percent` | Field weakening in reverse (100 = full field) | % | 80 – 100 |
| `Zero_RPM_Field_Boost_Percent` | Extra field current at zero/low RPM | % | 1 – 100 |
| `RPM_Field_Boost_Stop` | RPM at which field boost slopes down to 1x | RPM | 50 – 1000 |
| `Max_Field_Weaken_Amps` | Field current threshold for weakening | A | 0 – 10.00 |
| `Turbo_Start_RPM` | RPM at which field weakening begins | RPM | 1000 – 8000 |
| `Turbo_Weaken_Percent` | Field current reduction amount at turbo speed | % | 0 – 75 |

Factory defaults (read-only):

| Variable | Description | Unit |
|----------|-------------|------|
| `F_F_Table_Name` | Field table name (factory) | |
| `F_Reverse_Field_Weaken_Percent` | Reverse field weaken (factory) | % |
| `F_Zero_RPM_Field_Boost_Percent` | Zero RPM field boost (factory) | % |
| `F_RPM_Field_Boost_Stop` | RPM field boost stop (factory) | RPM |
| `F_Max_Field_Weaken_Amps` | Max field weaken current (factory) | A |
| `F_Turbo_Start_RPM` | Turbo start RPM (factory) | RPM |
| `F_Turbo_Weaken_Percent` | Turbo weaken percent (factory) | % |

### RAM Variables (Live State)

RAM variables reflect the controller's real-time state. They reset when the
controller loses power. Most are read-only monitoring values used by
`alltrax monitor`.

#### Error Flags (read-only)

Active error conditions. Each flag is a boolean — `true` means the error is
currently active. The controller may shut down or derate depending on which
flags are set.

| Variable | Description |
|----------|-------------|
| `Global_Shutdown` | Controller has shut down (any critical error) |
| `Hardware_Overcurrent` | Hardware overcurrent protection tripped |
| `OC_Retry_Running` | Overcurrent retry sequence in progress |
| `LOBAT` | Battery voltage below `LoBat_Vlim` |
| `HIBAT` | Battery voltage above `HiBat_Vlim` |
| `Precharge_Fail` | Output capacitor precharge failed |
| `Overtemp` | Controller temperature too high |
| `Undertemp` | Controller temperature too low |
| `Range_Alarm` | Throttle input out of expected range |
| `Hi_HThrot_Overrange` | High throttle input above maximum |
| `Lo_HThrot_Overrange` | High throttle input below minimum |
| `Hi_LThrot_Overrange` | Low throttle input above maximum |
| `Lo_LThrot_Overrange` | Low throttle input below minimum |
| `Field_Open_Alarm` | Field winding open circuit detected |
| `BMS_Missing` | BMS expected but not communicating |
| `AuxStuck` | Auxiliary output stuck (shorted or failed) |
| `Bad_Vars` | Settings page corrupt (GoodSet invalid) |

#### Raw ADC (read-only)

Raw analog-to-digital converter readings, before averaging. Useful for
diagnostics and throttle calibration.

| Variable | Description | Unit |
|----------|-------------|------|
| `Raw_Keyswitch` | Key switch voltage | V |
| `Raw_Reverse` | Reverse input voltage | V |
| `Raw_BatV` | Battery voltage (raw ADC) | V |
| `Raw_Temp` | Temperature sensor | C |
| `Raw_MotorHall` | Motor current hall sensor | |
| `Raw_F1Hall` | Field 1 current hall sensor | |
| `Raw_F2Hall` | Field 2 current hall sensor | |
| `Raw_ThrotHigh` | Throttle high input | |
| `Raw_ThrotLow` | Throttle low input | |
| `Raw_Footswitch` | Footswitch voltage | V |
| `Raw_Forward` | Forward input voltage | V |
| `Raw_User1` | User input 1 voltage | V |
| `Raw_User2` | User input 2 voltage | V |
| `Raw_User3` | User input 3 voltage | V |

#### Averaged ADC (read-only)

Averaged (filtered) analog readings. These are what the controller firmware
uses for control decisions.

| Variable | Description | Unit |
|----------|-------------|------|
| `Avg_Keyswitch` | Key switch voltage (averaged) | V |
| `Avg_Reverse` | Reverse input voltage (averaged) | V |
| `Avg_BatV` | Battery voltage (averaged) | V |
| `Avg_MotorHall` | Motor current hall sensor (averaged) | |
| `Avg_F1Hall` | Field 1 current hall sensor (averaged) | |
| `Avg_F2Hall` | Field 2 current hall sensor (averaged) | |
| `Avg_ThrotHigh` | Throttle high input (averaged) | |
| `Avg_ThrotLow` | Throttle low input (averaged) | |
| `Avg_Footswitch` | Footswitch voltage (averaged) | V |
| `Avg_Forward` | Forward input voltage (averaged) | V |
| `Avg_User1` | User input 1 voltage (averaged) | V |
| `Avg_User2` | User input 2 voltage (averaged) | V |
| `Avg_User3` | User input 3 voltage (averaged) | V |

#### Monitoring (read-only)

Real-time controller status. These are the variables read by `alltrax monitor`.

| Variable | Description | Unit |
|----------|-------------|------|
| `Keyswitch_Input` | Key switch on/off | bool |
| `Reverse_Input` | Reverse switch on/off | bool |
| `Relay_On` | Main contactor relay state | |
| `Fan_Status` | Cooling fan running | bool |
| `HPD_Ran` | High Pedal Disable has triggered | bool |
| `Bad_Vars_Code` | Settings corruption diagnostic code | |
| `Footswitch_Input` | Footswitch on/off | bool |
| `Forward_Input` | Forward switch on/off | bool |
| `User1_Input` | User input 1 on/off | bool |
| `User2_Input` | User input 2 on/off | bool |
| `User3_Input` | User input 3 on/off | bool |
| `Charger_Plugged_In` | Charger detected | bool |
| `Blower_Status` | Blower motor running | bool |
| `Check_Motor_Status` | Motor check warning active | bool |
| `Horn_Status` | Horn output active | bool |
| `Avg_Temp` | Controller temperature | C |
| `BPlus_Volts` | Battery voltage | V |
| `KSI_Volts` | Key switch input voltage | V |
| `Throttle_Local` | Throttle input (raw machine units) | MU |
| `Throttle_Position` | Throttle position after linearization | MU |
| `Output_Amps` | Motor output current | A |
| `Field_Amps` | Field winding current | A |
| `Throttle_Pointer` | Throttle setpoint (what the controller is targeting) | MU |
| `Overtemp_Cap` | Over-temperature current derating level | |
| `Speed` | Motor speed (requires speed sensor) | RPM |
| `State_Of_Charge` | Battery state of charge estimate | % |
| `Battery_Amps` | Battery current (input side) | A |

#### Controls (writable RAM)

Writable RAM variables for real-time control. These take effect immediately
but do not persist across power cycles.

| Variable | Description | Unit |
|----------|-------------|------|
| `RunMode` | Controller mode: 0 = RUN (normal), 255 = CAL (settings mode). Used internally by the write bracket — you normally don't need to write this directly. | |
| `Fan_On` | Fan test trigger. Writing `1` starts a timed fan test (~10–30 seconds); the firmware stops it automatically. This is a momentary command, not a toggle — reading it always returns 0. | bool |

## Supported hardware

Tested on:

- **Alltrax XCT48400-DCS** (USB PID `0x0002`, firmware V5.005)

Should work with other XCT-series controllers sharing the same USB Product ID.
SPM and SR series use different memory maps and are not yet supported.

## How it works

Alltrax controllers expose a USB HID interface. The host reads and writes
directly to ARM memory addresses on the STM32 microcontroller inside the
controller — there is no abstraction layer. Settings live in FLASH at fixed
addresses determined by the USB Product ID.

## License

LGPL-3.0-or-later. See [LICENSE](LICENSE).
