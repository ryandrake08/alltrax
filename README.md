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
# Write a single FLASH setting (controller reboots to apply)
alltrax write LoBat_Vlim=28.1

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

## Library

The C library (`liballtrax-usb.a`) has a clean public API in
[`src/alltrax.h`](src/alltrax.h). The CLI is a thin wrapper around it.

```c
#include "alltrax.h"

alltrax_controller* ctrl;
alltrax_error err = alltrax_open(&ctrl, false);

alltrax_info info;
alltrax_get_info(ctrl, &info);
printf("Model: %s\n", info.model);

alltrax_close(ctrl);
```

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
