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

### Flags

| Flag | Effect |
|------|--------|
| `--no-cal` | Skip CAL/RUN mode bracket (safe for bench programming with no motor) |
| `--no-verify` | Skip read-back verification after FLASH write |
| `--force` | Allow writes on unrecognized firmware versions |
| `--reset` | Reboot controller after FLASH write (`write` only) |

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
    const alltrax_var_def** vars, const double* values, size_t count);
alltrax_error alltrax_write_flash_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count);
```

Both functions take arrays of variable definitions and display-scale values.
Values are automatically converted from display units to raw device format.

**RAM writes** take effect immediately but are not persistent across power
cycles. The library enters CAL mode, writes all variables, then restores RUN
mode.

**FLASH writes** are persistent. The library performs a full page cycle: read
the 2KB settings page, patch in the new values, erase the page, write it back,
and verify. Multiple variables are batched into a single page cycle. The
controller requires `allow_writes=true` and firmware V5.005. Only variables
with `is_flash=true` can be written with this function.

Example:

```c
const alltrax_var_def* vars[] = {
    alltrax_find_var("LoBat_Vlim"),
    alltrax_find_var("HiBat_Vlim"),
};
double values[] = { 28.1, 61.0 };
alltrax_write_flash_vars(ctrl, vars, values, 2);
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
