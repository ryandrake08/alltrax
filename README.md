# alltrax

Open source CLI tool and C library for communicating with
[Alltrax](https://www.alltraxinc.com/) XCT-family motor controllers
via USB HID.

Please note: This software is experimental, entirely unofficial, and not
affiliated with the device manufacturer, Alltrax, in any way. It is a hobby
project written by an enthusiastic end user.

```
$ alltrax info
Controller Type:  XCT
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
| `alltrax devices` | List responsive controllers by HID path, model, and serial number |
| `alltrax get [--factory] [var...]` | Read settings (all or by name) |
| `alltrax write var=value [...]` | Write settings |
| `alltrax reset` | Reboot controller and wait for it to reconnect |
| `alltrax clear-autocal` | Clear learned throttle calibration and reboot |
| `alltrax restore-defaults --throttle --field-map` | Restore factory settings and reboot |
| `alltrax fan-test` | Start a timed fan test with the keyswitch on |
| `alltrax monitor [--rate <ms>] [--log <file.csv>]` | Live controller readings (Ctrl+C to stop) |
| `alltrax monitor throttle` | Live throttle readings (Ctrl+C to stop) |
| `alltrax errors` | Active error flags, and the errors active in the last 10 seconds with how long ago (keyswitch on) |
| `alltrax config save [flags] <file>` | Save all settings to file |
| `alltrax config load [flags] <file>` | Load settings from file and write to device |
| `alltrax curve list` | List available curve types |
| `alltrax curve get <type>` | Read curve table from device |
| `alltrax curve set <type>` | Write curve table to device |
| `alltrax curve diff <type>` | Compare user vs factory curve |
| `alltrax curve reset <type>` | Restore factory curve |
| `alltrax throttle list` | List throttle labels supported by the connected controller; `*` marks the current one |
| `alltrax throttle set <name>` | Select a throttle and reboot |
| `alltrax field-map history` | List saved field-map names, dates, and the total map count |
| `alltrax field-map list <file>` | List the maps in a field-map file and any upgrade for the controller |
| `alltrax field-map upgrade <file>` | Install a newer version of the controller's field map |
| `alltrax field-map install <file> <name>` | Install a named field map and its speed sensor pole count |

Command options may appear before or after arguments. Unknown options, options
a command does not accept, missing option values, and extra arguments are
errors, reported before the device is opened. `curve set` takes exactly one
point source: x,y pairs, `--file`, or `--preset`.

### Firmware policy

| Firmware (boot and program) | `info` | Other reads | Writes and `reset` |
|---|---|---|---|
| V5.005 (validated for writes) | Yes | Yes | Yes |
| Other V0.001-V5.999 | Yes | Yes | With `--allow-unvalidated-firmware` |
| Outside V0.001-V5.999 | Yes | With `alltrax --debug-ignore-fw-bounds <command>` | Never |

`--allow-unvalidated-firmware` is accepted by the write commands (`write`,
`reset`, `clear-autocal`, `restore-defaults`, `fan-test`, `config load`,
`curve set`, `curve reset`, `throttle set`).
`--debug-ignore-fw-bounds`
is a debug option placed before the command; it affects reads only.

### Device and settings commands

```sh
alltrax devices
alltrax --device /dev/hidraw1 info
alltrax get LoBat_Vlim
alltrax get --factory LoBat_Vlim
alltrax fan-test
alltrax clear-autocal
alltrax reset
```

Use the HID path from `devices` with the global `--device` option to select
a controller. `devices` exits with status 1 when it finds no controller. `get` warns if the user settings page is invalid. To read
the factory page, add `--factory` and use the same setting names. This
fails if the factory page is invalid.

`fan-test` requires the keyswitch (KSI) to be on. The firmware ends the
test automatically. `clear-autocal` clears learned throttle calibration
and sends one reset. `clear-autocal`, `reset`, and any write command that
resets the controller wait up to 7.5 seconds for it to reconnect, so a
following command finds it ready. If USB passthrough leaves the device node stale, the command
reports a warning and exits successfully after the reset was sent. Unplug
and reconnect the USB cable to restore access in that case.

### Restore factory defaults

```sh
alltrax config save backup.txt        # optional: keep a copy to restore from
alltrax restore-defaults --throttle --field-map
```

`restore-defaults` replaces all user settings, including the throttle setup
and the field map, with the controller's factory settings. It checks that the
factory page is valid before writing, and requires `--throttle` and
`--field-map` because it replaces the throttle settings and the field map (see
[Throttle and field-map settings](#throttle-and-field-map-settings)). To bring back some of your settings afterwards, load
a config file that holds just those settings with `config load`. The result
is written in one verified page write. Throttle auto-calibration is cleared
when the throttle type or HPD changes.

The command updates the field-map date and sends one reset on success.
Before using the controller,
check that the field map matches the installed motor and that the speed
sensor pole count is correct.

### Live monitor

```sh
alltrax monitor
alltrax monitor --rate 1000 --log drive.csv
alltrax monitor throttle
```

The full monitor targets 100 ms between samples by default. Use `--rate 1000`
or `--rate 3000` for slower polling. USB response times can limit the actual
rate. `--log` appends a session header with the model, serial number, and
field-map name, then adds one CSV row per sample. The columns follow the
Toolkit's monitor log and include an extra UTC timestamp with milliseconds.

The throttle view targets 100 ms and shows input, linearized throttle,
setpoint, and output percentages. The percentages are blank with the
keyswitch off. Stop either view with Ctrl+C.

### Throttle selection

```sh
alltrax throttle list
alltrax throttle set "0-5K 2 Wire"
alltrax write HPD=0          # optional: turn high pedal disable off afterwards
```

Choose a label from the connected controller's list. Names are case
insensitive, and an unambiguous prefix also works. The stored label is
limited to 16 characters. In the list, `*` marks the current throttle: the
entry of the stored type whose label starts with the stored name, otherwise
the first entry of that type.

`throttle set` writes the throttle type, its label, its HPD default and its
default linearization curve, even when the same type is already selected. To
keep a custom curve, load it again afterwards with `curve set` or
`config load`. High pedal disable (HPD) is turned on, except for Digital, Pump
and USB, where it is always off. To turn HPD off for another throttle, write
`HPD=0` afterwards (a second settings write and reboot).
The command clears learned throttle calibration and sends one reset. For an absolute
throttle, verify its calibration before operating the controller.

### Write examples

`alltrax write [options] var=value ...` takes one or more assignments. Each
variable may appear once per batch.

```sh
# Write a single FLASH setting (the controller reboots to apply it)
alltrax write LoBat_Vlim=28.1

# Batch write — multiple settings in one FLASH page cycle
alltrax write LoBat_Vlim=28.1 HiBat_Vlim=61.0

# Write a RAM variable (takes effect immediately, not persistent)
alltrax write Fan_On=1

# Write a string setting together with a number (one FLASH page cycle)
alltrax write Throttle_Type_Name="0-5K 2 Wire" HPD=1

# Clear a string setting (stored as spaces)
alltrax write Throttle_Type_Name=

# Set a current in amps
alltrax write N_Max_Arm_Motor_Amps=250A

# Set two related values in the same batch
alltrax write Speed_Limit_On=1 N_Speed_Limit=4500
```

Quote string values that contain spaces. Strings accept printable ASCII up
to the field size and ignore trailing spaces.

`write` validates the batch against the controller's hardware, firmware, and
batch values before making changes.

After writing to the settings page, the command reboots the controller once
so the firmware loads the new settings. A batch that writes only the
linearization curve and field-map data (the Field settings and the `field`
and `genfield` curves) does not reboot. RAM writes (such as `Fan_On`) take
effect immediately and do not reboot. The rule depends on which settings are
in the batch, not on whether their values change. Use `alltrax reset` to
reboot at another time.
See the [variable reference](#variable-reference) for units, limits, and
side effects.
The command prints each action it performed, such as "Cleared throttle
auto-calibration" and "Device reset sent", including after a failed write.

### Value rules

`write`, `curve set`, `config load`, `field-map install` and `field-map
upgrade` refuse values the controller would store but that are unsafe,
meaningless on a vehicle, or outside the controller's rating:

| Rule | Why |
|------|-----|
| Throttle curves (`linearization`, `speed`, `torque`) start at 0,0 and end at 100 % X | No output with the pedal released; every pedal position is covered |
| Throttle curve X rises by at least 1 % per point and Y never falls | More pedal never gives less output |
| Field-map curves (`field`, `genfield`) start at X 0, end at 750 A X and rise strictly in X | Every field current is covered; Y may fall |
| `N/U1/U2_RollDetect_BrakingCurrent` is at least 10 A | Keeps roll-detect braking from being set to almost nothing |
| `N/U1/U2_Throttle_Rate` and `*_Throttle_Rate_Max` are at most 100 MU | Caps how fast the throttle responds |
| `Speed_Limit_On=1` needs a speed sensor | Speed limits are in RPM, measured by the speed sensor |
| Voltage settings lie within 16.0–62.0 V (48 V controllers) or 20.0–90.0 V (72 V); other ratings cannot set them | Keeps cutoffs within the controller's rating |
| `AnalogInputs_Threshold` + 1 V ≤ `LoBat_Vlim`; `LoBat_Vlim` + 1 V ≤ `HiBat_Vlim` and ≤ `BMS_Missing_HiBat_Vlim` ≤ `HiBat_Vlim` | Keeps the cutoffs in order. Values not in the batch are read from the settings the write changes (the factory settings while the user settings are invalid) |
| `Throttle_Type` is one the hardware allows | |
| In `UserInputs_State` 1 or 2, each live range runs from the plain setting up to its `_Max` partner: `*_Speed_Limit`, `*_Throttle_Rate`, `*_Forward_Speed` ≤ their `_Max`, `*_Max_Arm_Regen_Amps` no larger in magnitude than its `_Max`, and no `_Max` left unset (−1). Mode 1 uses the Normal, User 1 and User 2 profiles, mode 2 the Normal profile | In these modes the analog user input selects a value within each range. Values not in the batch are read from the controller |

Trailing 0,0 curve points are unused and ignored. Factory data and built-in
presets (`curve reset`, `restore-defaults`, `throttle set`) are not checked.
`--skip-rules` skips the rules and prints a warning.

Before a FLASH write, the tool checks both settings pages. If the factory
page is invalid, it refuses the write. If only the user settings page is
invalid, it restores the factory page to user settings, applies the
requested changes, and reports the recovery.

### User Inputs ranges

`UserInputs_State` selects how the controller's two user inputs are used:
0 for on/off switches that select the User 1 and User 2 profiles, 1 and 2
for analog inputs (an Alltrax function box). In modes 1 and 2 the speed
limit, throttle rate, regen armature current and forward speed of each
profile become ranges: the plain setting is the low end, its `_Max` partner
the high end, and the analog input picks a point in between. In mode 0 only
the plain setting is used and the `_Max` values have no effect.

The tool never changes these values on its own; set both ends explicitly, for
example `alltrax write UserInputs_State=1 N_Speed_Limit=1000
N_Speed_Limit_Max=3000`. Besides the rule above:

- A write that switches into mode 1 or 2 lists every range that becomes live,
  so values left over from earlier use are visible before the controller
  uses them.
- A write that switches back to mode 0 warns about each profile whose plain
  setting is below its `_Max`, because mode 0 uses the plain setting.

### Write options

| Option | Effect |
|--------|--------|
| `--allow-unvalidated-firmware` | Allow writes on unvalidated firmware within V0.001-V5.999 |
| `--skip-rules` | Skip the [value rules](#value-rules) (`write`, `curve set`, `config load`, `field-map install`, `field-map upgrade`) |

Debug options are listed under [Debug and development options](#debug-and-development-options).

### Throttle and field-map settings

Two groups of settings must match the installed hardware:

- **Throttle settings** (`Throttle_Type`, `Throttle_Type_Name`, `HPD`, the
  `ABS_*` calibration values, and the `linearization`, `speed` and `torque`
  curves) must match the installed throttle.
- **The field map** (`F_Table_Name`, the six field weakening settings, and the
  `field` and `genfield` curves) must match the installed motor; a wrong map
  can damage the motor.

Commands that name what they write need no confirmation: `write`,
`curve set`, `curve reset` of one curve, `throttle set`, and `field-map
upgrade` and `install`. Bulk commands write these groups only when you
confirm each one: `--throttle` for throttle settings and `--field-map` for the
field map. A bulk command missing a confirmation refuses before the controller
is opened and names what is missing; it never writes only part of what you
asked for.

| Bulk command | Needs |
|--------------|-------|
| `config load <file>` | `--throttle` if the file has throttle settings, `--field-map` if it has field-map settings |
| `curve reset all` | `--throttle --field-map` |
| `restore-defaults` | `--throttle --field-map` |

Field-map writes print a warning and afterwards remind you to check the speed
sensor pole count (`Tach_4_8`). They always include `F_Table_Name`: when a
command does not set it, the name from the settings page used for the write
is written back with the map.

```sh
alltrax write Turbo_Start_RPM=2500
alltrax curve reset field
alltrax config load --throttle --field-map backup.txt
```

### Field maps

`alltrax field-map history` reads the controller's history on XCT-family
controllers other than SRX with program firmware newer than V2.002. The total
map count includes older entries that rolled off the history page.

#### Field-map files

`list`, `upgrade` and `install` read a field-map file in the Alltrax Toolkit's
format. The file is not included with the CLI. It holds a version line and a
series of maps; each map has a name, an integer version, optional alias
names, and the map's settings. Maps marked hidden are ignored. A malformed map
entry rejects the whole file before the controller is opened. These commands
need libxml2.

```sh
alltrax field-map list fields.txt
alltrax field-map upgrade fields.txt
alltrax field-map install fields.txt MOTOR-MAP
```

- `list` prints every map with its version and aliases, marks the controller's
  map with `*`, and reports whether an upgrade is available.
- `upgrade` installs a newer version of the controller's current map. The
  controller's map name is `F_Table_Name` up to `-V`, and the number after
  `-V` is its version (0 when absent). The upgrade is the last map in the file
  that has the same name and a higher version, or that lists the name as an
  alias; it is installed only when its version is higher than the
  controller's. The controller's user settings must
  be valid (Var_GoodSet = 0). The speed sensor pole count (`Tach_4_8`) is kept. If the installed map is the
  universal map (UNIMAP), the command says so.
- `install` writes the named map, or the highest version of it when the file
  has several. A name that matches no map is looked up among the aliases. It
  prints the old and new speed sensor pole count and writes `Tach_4_8` with
  the map, which reboots the controller.

Both writes go through the same batch write as `config load`: one CAL/RUN
bracket and one settings page erase, followed by the field-map date update.
They accept the [write options](#write-options).

`--key` selects how the file is decrypted:

- `auto` (the default) uses the key in the `ALLTRAX_FIELDMAP_KEY` environment
  variable, otherwise the field-map key built into the CLI. If neither is
  available, the command fails before the controller is opened.
- `<key>:<iv>` uses the given 3DES key: 48 hex digits, a colon, and 16 hex
  digits (either case). It overrides `ALLTRAX_FIELDMAP_KEY`.
- `none` reads a plain text file.

`ALLTRAX_FIELDMAP_KEY` holds a `<key>:<iv>` value only; `auto`, `none`, or a
malformed value there is an error, and an empty value is ignored. Malformed
`--key` values also fail before the controller is opened. The usage text of
`list`, `upgrade` and `install` shows whether a key is built in.

### Config save/load

Save and restore controller settings to a file. By default, files are 3DES
encrypted for compatibility with the Windows Alltrax Toolkit. Use
`--key none` for plain XML.

```sh
# Save settings (encrypted, Toolkit-compatible)
alltrax config save settings.txt

# Save settings (plain XML, human-readable)
alltrax config save --key none settings.xml

# Load settings from file and write to controller
alltrax config load --throttle --field-map settings.txt

# Load plain XML
alltrax config load --key none --throttle --field-map settings.xml
```

`config save` writes every writable setting and curve, including the field
map. Strings are space-padded to their field size, and read-only values such
as factory defaults are not saved. It writes a file only when every settings
group and curve has been read. It refuses to save when the controller's user
settings are not valid (Var_GoodSet ≠ 0): either only factory defaults are
present, or no settings are loaded. Read, validity, and encryption failures
exit with an error and leave an existing file unchanged. If writing the file
itself fails (for example, a full disk), it exits with an error and removes
the partial file; the previous contents are not kept.

`config load` restores every writable setting in the file, including the
`Throttle_Type_Name` string and all curves. A file with throttle or field-map
settings needs `--throttle` or `--field-map` (see
[Throttle and field-map settings](#throttle-and-field-map-settings)). Writes
everything in one batch: one CAL/RUN bracket and one settings page erase.
Read-only and unknown entries are skipped. Any invalid value rejects the load
before the controller is opened: a number that is not a whole integer, a
boolean other than 0 or 1, a curve without exactly 16 values, or a string
that is too long or not ASCII.

Config files saved by the Alltrax Toolkit ("Save All Settings") can be loaded
with `alltrax config load`. Config files saved by the CLI can be loaded by the
Toolkit (unless `--key none` was used).

`--key` selects how files are encrypted:

- `auto` (the default) uses the key in the `ALLTRAX_CONFIG_KEY` environment
  variable, otherwise the key built into the CLI. If neither is available,
  the command fails before the controller is opened.
- `<key>:<iv>` uses the given 3DES key: 48 hex digits, a colon, and 16 hex
  digits (either case). It overrides `ALLTRAX_CONFIG_KEY`.
- `none` reads and writes plain XML. This is the only way to skip encryption.

`ALLTRAX_CONFIG_KEY` holds a `<key>:<iv>` value only; `auto`, `none`, or a
malformed value there is an error, and an empty value is ignored. Malformed
`--key` values also fail before the controller is opened. `config save`
reports whether the file was written encrypted or as plain XML, and the usage
text of `config save` and `config load` shows whether a key is built in.

| Option | Applies to | Effect |
|--------|-----------|--------|
| `--key auto\|<key>:<iv>\|none` | save, load | Encryption key, or plain XML with `none` |
| `--allow-unvalidated-firmware` | load | Allow writes on unvalidated firmware within V0.001-V5.999 |
| `--throttle` | load | Confirm writing the file's throttle settings |
| `--field-map` | load | Confirm writing the file's field-map settings |
| `--skip-rules` | load | Skip the [value rules](#value-rules) |

### Curve tables

The controller uses 16-point curve tables to control throttle linearization,
speed/torque limits, and field weakening. Each curve is a pair of X/Y arrays
(16 int16 values each) stored in FLASH. The `curve` command reads and writes
these tables independently from the scalar variable system.

**Curve types:**

| Type | Description | X/Y Unit |
|------|-------------|----------|
| `linearization` | Throttle linearization (input → output mapping) | % / % |
| `speed` | Speed limit curve | % / % |
| `torque` | Torque limit curve | % / % |
| `field` | Field weakening curve | A / A |
| `genfield` | Generator field curve | A / A |
| `all` | All 5 curves (get and diff only) | |

**Presets** (built-in curves, use with `--preset`):

| Curve | Preset | Description |
|-------|--------|-------------|
| `linearization` | `linear` | Simple linear response |
| `linearization` | `0-5k-2wire` | S-curve for 0-5K ohm 2-wire pot |
| `linearization` | `5k-0-2wire` | Inverse S-curve for 5K-0 ohm 2-wire pot |
| `linearization` | `yamaha` | Compressed range for Yamaha 0-1K throttle |
| `linearization` | `clubcar` | Offset start for Club Car 3-wire throttle |
| `speed` | `standard` | Standard linear speed curve |
| `torque` | `standard` | Aggressive XCT torque curve |

```sh
# List available curve types
alltrax curve list

# Read a curve and print as table
alltrax curve get linearization

# Read all curves
alltrax curve get all

# Read a curve with ASCII plot
alltrax curve get linearization --plot

# Export curve to CSV file
alltrax curve get linearization --file lin.csv

# Export all curves to one CSV file
alltrax curve get all --file curves.csv

# Write curve from x,y pairs (1-16 points, remaining padded with 0,0)
alltrax curve set linearization 0,0 0.2,0 2.4,7.8 10,20 50,50 100,100

# Write curve from CSV file
alltrax curve set linearization --file lin.csv

# Write a built-in preset curve
alltrax curve set linearization --preset 0-5k-2wire
alltrax curve set torque --preset standard

# Write from a CSV file
alltrax curve set linearization --file lin.csv

# Compare user curve to factory defaults (differences marked with *)
alltrax curve diff linearization

# Compare all curves
alltrax curve diff all

# Reset curve to factory defaults
alltrax curve reset linearization

# Reset all curves to factory defaults (includes the field map)
alltrax curve reset all --throttle --field-map
```

`curve reset` reads every selected factory curve first, then writes them all
in one batch (one settings page erase), so a failed read changes nothing.
`curve reset all` needs `--throttle --field-map`.
Writing the `speed` or `torque` curve reboots the controller once afterwards;
writing only the `linearization`, `field` or `genfield` curves does not.

**CSV format:** One `x, y` pair per line. Lines starting with `#` are comments.
Blank lines are ignored. Up to 16 points; if fewer are provided, the remaining
points are set to 0,0.

Curve points must be finite and round into the supported raw bounds: 0–4095
for both throttle axes (0–100%), and 0–7500 X / 0–5000 Y for field curves
(0–750 A / 0–50 A). Zero padding and repeated points are allowed. Malformed
values and more than 16 points are rejected. Config import validates every
curve before applying any settings. Malformed or incomplete curve arrays
cause the import to fail.

```csv
# Linearization curve
# X (%), Y (%)
0.0000, 0.0000
0.2441, 0.0000
2.4410, 7.8144
10.0000, 20.0000
50.0000, 50.0000
100.0000, 100.0000
```

| Flag | Applies to | Effect |
|------|-----------|--------|
| `--file <path>` | get, set | CSV file for import/export |
| `--preset <name>` | set | Use a built-in preset curve |
| `--plot` | get | ASCII chart instead of table |
| `--allow-unvalidated-firmware` | set, reset | Allow writes on unvalidated firmware within V0.001-V5.999 |
| `--throttle`, `--field-map` | reset all | Confirm resetting the throttle curves and the field map |
| `--skip-rules` | set | Skip the [value rules](#value-rules) |

### Debug and development options

These exist to diagnose problems and develop this tool, not for normal use,
and are listed under their own heading in the `alltrax` usage message.

| Option | Applies to | Bypasses |
|--------|-----------|----------|
| `--debug-ignore-fw-bounds` | global (before the command) | Firmware bounds for reads (firmware outside V0.001-V5.999); never widens writes |

`alltrax debug-read <address> [length]` prints a hex dump of controller
memory. It opens the controller read-only and never writes. Reads are limited
to RAM (0x20000000-0x2000FFFF), FLASH (0x08000000-0x0801FFFF) and the backup
registers (0x40006C00-0x40006C13); other addresses are refused before the
device is opened, because reading some peripheral registers changes their
state and reading an unmapped address faults the controller. The length
defaults to 4 bytes, at most 2048. Numbers are decimal or 0x-prefixed
hexadecimal. A 2- or 4-byte read also prints its little-endian value.
`--watch <ms>` (100-60000) keeps the device open and reads again at that
interval until Ctrl+C, starting each line with the seconds since the first
read.

```sh
alltrax debug-read 0x40006C00 20
alltrax debug-read 0x40006C10 --watch 1000
```

```
0x40006C00  00 00 00 00 00 00 00 00 78 05 00 00 d6 06 00 00
0x40006C10  23 08 00 00
```

## Building

### Dependencies

Required:

- C11 compiler (gcc or clang)
- GNU Make and pkg-config
- [hidapi](https://github.com/libusb/hidapi) — USB HID communication. `make`
  stops with an error naming the package if pkg-config cannot find it.

Optional (used only by `config save`, `config load`, and the `field-map`
file commands):

- [libxml2](https://gitlab.gnome.org/GNOME/libxml2) — XML parsing and generation
- [OpenSSL](https://www.openssl.org/) libcrypto — 3DES encryption for
  Toolkit-compatible config and field-map files

`make` always builds the library, tests, and CLI, and prints a note for each
missing optional library:

- Without libxml2, the `config` command and `field-map list`, `upgrade` and
  `install` report that they are unavailable.
- Without libcrypto, `config save` and `config load` handle plain XML only and
  require `--key none`, as do the `field-map` file commands; encrypted files and the built-in keys are not
  available.

Installing a missing library and running `make` again rebuilds the affected
files; `make clean` is not needed.

**Linux (Debian/Ubuntu):**

```sh
# All features
sudo apt install build-essential pkg-config libhidapi-dev libssl-dev libxml2-dev

# Without config files
sudo apt install build-essential pkg-config libhidapi-dev
```

**macOS (MacPorts):**

```sh
sudo port install hidapi openssl libxml2
```

**macOS (Homebrew):**

```sh
brew install hidapi openssl libxml2
```

### Compile

```sh
make            # builds CLI + library + tests (or library + tests if deps missing)
make test       # runs tests
make clean      # removes build/
make EMBEDDED_KEYS=0   # builds without the built-in config and field-map keys
                       # (build/<variant>-nokeys/)

make DEBUG=1 test      # debug build with AddressSanitizer (build/debug/)
make SANITIZE=1 test   # AddressSanitizer + UndefinedBehaviorSanitizer,
                       # including float-cast-overflow (build/sanitize/)
make CC=clang-22 VARIANT=clang test   # clang in its own build directory
```

The automated tests never access USB hardware; they use simulated transports.
In a build with the built-in keys, `make test` also runs the CLI tests
against config and field-map handling compiled without them.

## USB permissions

The controller appears as a HID device (`/dev/hidraw*`). By default this
requires root access. To give members of the `plugdev` group access, create a
udev rule:

```sh
# /etc/udev/rules.d/60-alltrax.rules
KERNEL=="hidraw*", ATTRS{idVendor}=="23d4", ATTRS{idProduct}=="0002", MODE="0660", GROUP="plugdev"
```

Then reload the rules and add your account to the group (log in again for
the group change to take effect):

```sh
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG plugdev "$USER"
```

On macOS, no extra permissions are needed — HID devices are accessible to
regular users.

Only one `alltrax` command can use the controllers at a time. While one runs,
for example `alltrax monitor`, another command stops with "Controller in use
by another alltrax process" before sending anything. Two programs talking to
the controller at once would receive each other's replies.

## Library API

The C library (`liballtrax-usb.a`) provides the full public API. The CLI is a
thin wrapper around it. Link with `-lalltrax-usb -lhidapi-hidraw -lm`.

### Quick start

```c
#include "alltrax.h"

alltrax_error err = alltrax_init();
if (err) { fprintf(stderr, "%s\n", alltrax_strerror(err)); return 1; }

alltrax_controller* ctrl = NULL;  /* alltrax_open() requires NULL */
err = alltrax_open(&ctrl, NULL, false);
if (!err) {
    alltrax_info info;
    err = alltrax_get_info(ctrl, &info);
    if (!err)
        printf("Model: %s, Firmware: %s\n", info.model, info.program_rev_str);
    alltrax_close(ctrl);
}
if (err)
    fprintf(stderr, "%s\n", alltrax_strerror(err));

alltrax_exit();
return err ? 1 : 0;
```

### Lifecycle

```c
alltrax_error alltrax_init(void);
void          alltrax_exit(void);
```

Call `alltrax_init()` once before any other library call. Call `alltrax_exit()`
after all controllers are closed. These manage the underlying USB HID library.

```c
alltrax_error alltrax_open(alltrax_controller** out,
    const char* device_path, bool allow_writes);
void          alltrax_close(alltrax_controller* ctrl);
```

`alltrax_open()` connects to an enumerated Alltrax XCT-family device
(VID `0x23d4`, PID `0x0002`). Pass `NULL` for automatic detection or an OS HID
path that matches a supported enumeration entry. Opening performs a read-only
model check and accepts the XCT, SRX, and NCT prefixes. PID 1 and unrecognized
families are rejected with `ALLTRAX_ERR_UNSUPPORTED`; unmatched paths return
`ALLTRAX_ERR_NO_DEVICE`. Firmware overrides cannot bypass these checks.
Opening also reads the boot and program revisions and StartUpSwitch.
`*out` must be `NULL` on entry and remains `NULL` on failure. Set `allow_writes`
to `true` if you intend to write settings; the library will reject write
operations otherwise. Multiple controllers can be open simultaneously.

```c
alltrax_error alltrax_set_firmware_read_override(alltrax_controller* ctrl,
    bool enabled);
```

Firmware policy: `alltrax_get_info()` works on any firmware. Other reads
require boot and program revisions within V0.001-V5.999 and otherwise return
`ALLTRAX_ERR_FIRMWARE`; enabling the read override allows them. Writes and
reset require write-validated firmware (V5.005); `skip_fw_check` widens this to
V0.001-V5.999. Firmware outside that range is never written, and the read
override does not affect writes.

`info` reports whether the controller is in program or bootloader mode. In
bootloader mode, only controller information can be read. Other controller
operations return `ALLTRAX_ERR_BOOTLOADER`, even with a firmware override.

### Error handling

All functions that can fail return `alltrax_error`:

| Code | Meaning |
|------|---------|
| `ALLTRAX_OK` | Success |
| `ALLTRAX_ERR_USB` | HID communication failure |
| `ALLTRAX_ERR_TIMEOUT` | No response from device |
| `ALLTRAX_ERR_PROTOCOL` | Malformed or incomplete response |
| `ALLTRAX_ERR_DEVICE_FAIL` | Device reported failure |
| `ALLTRAX_ERR_VERIFY` | Write echo mismatch |
| `ALLTRAX_ERR_FIRMWARE` | Unsupported firmware version |
| `ALLTRAX_ERR_FLASH` | FLASH operation failed |
| `ALLTRAX_ERR_BOOTLOADER` | Settings and reset unavailable in bootloader mode |
| `ALLTRAX_ERR_INVALID_ARG` | Bad argument |
| `ALLTRAX_ERR_NO_DEVICE` | No Alltrax device found |
| `ALLTRAX_ERR_UNSUPPORTED` | Unsupported USB identity or controller family |
| `ALLTRAX_ERR_BLOCKED` | Write blocked (`allow_writes=false`) |
| `ALLTRAX_ERR_NO_MEMORY` | Memory allocation failed |
| `ALLTRAX_ERR_BUSY` | Another handle or process holds the controller lock |
| `ALLTRAX_ERR_LOCK` | The lock file `/tmp/alltrax.lock` cannot be opened or locked |

HID transfers require exactly 64 bytes, including the Report ID. An incomplete
write returns `ALLTRAX_ERR_USB`; a positive-length incomplete read returns
`ALLTRAX_ERR_PROTOCOL` before parsing. The reply timeout is 750 ms. After a
timeout, the library drains pending reports, resends once, and drains again
before moving on. Negative HID writes get up to four attempts, 125 ms apart.
A reset is sent once because it has no reply. Transfer-length diagnostics
report the expected and actual byte counts.

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
capabilities for an accepted XCT-family device, on any firmware. Check
`out->supported` to determine whether other reads are allowed without the read
override, and `out->write_validated` to determine whether writes are allowed
without `skip_fw_check`. Unsupported USB identities and controller families are
rejected centrally, including by `alltrax_get_info()`; PID 1 identification
is not implemented.

**`alltrax_info` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `controller_type` | `alltrax_controller_type` | Detected type (XCT, SPM, SR, BMS, BMS2, or Unknown) |
| `supported` | `bool` | `true` when boot and program revisions are within V0.001-V5.999 |
| `write_validated` | `bool` | `true` when the program revision is validated for writes (V5.005) |
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

Checks whether the firmware is new enough to have reliable hardware config
bytes for a given feature. The `alltrax_info` struct fields (e.g.
`has_forward`, `throttles_allowed`) are already gated by `alltrax_get_info()`
— old firmware gets defaults (capabilities assumed present; every throttle
type allowed except EZGO ITS). This
function is useful if you need to know whether the value was read from
hardware config vs. defaulted.

| Feature | Condition | Description |
|---------|-----------|-------------|
| `ALLTRAX_FEAT_HW_CAPS` | `orig_boot != 1 \|\| orig_prgm != 1` | BMS CAN, throttle CAN, User2/3, AuxOut1/2 bytes valid |
| `ALLTRAX_FEAT_THROTTLE_CAPS` | `orig_boot > 2 \|\| orig_prgm > 2` | Throttles allowed bitmask valid |
| `ALLTRAX_FEAT_FORWARD_INPUT` | `orig_prgm >= 68 \|\| prgm_ver == 200` | Forward input byte valid |
| `ALLTRAX_FEAT_USER1_INPUT` | `orig_prgm >= 70 \|\| prgm_ver == 200` | User 1 input byte valid |
| `ALLTRAX_FEAT_USER_PROFILES` | `program_rev >= 1005` | User profile switching supported |
| `ALLTRAX_FEAT_USER_DEFAULTS` | `orig_prgm >= 1007` | User-restorable defaults supported |
| `ALLTRAX_FEAT_CAN_HIGHSIDE` | `orig_prgm >= 1008` | High-side drive / stock mode bytes valid |
| `ALLTRAX_FEAT_BAD_VARS_CODE` | `program_rev >= 1107` | Bad vars diagnostic code available |

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
| `size` | `uint8_t` | String length 1–32 (0 for non-string types) |
| `scale` | `double` | Display scaling: `display = (raw - offset) * scale` |
| `offset` | `int32_t` | Raw value offset before scaling |
| `unit` | `const char*` | Display unit (e.g. `"V"`, `"A"`, `"%"`) or `NULL` |
| `writable` | `bool` | Whether this variable can be written |
| `is_flash` | `bool` | `true` = FLASH (persistent), `false` = RAM |

Device operations accept only the library's variable entries: the pointers
returned by `alltrax_find_var()` and `alltrax_get_var_defs()`. Any other
pointer, including a copy of an entry, returns `ALLTRAX_ERR_INVALID_ARG`
before device I/O and rejects the entire batch, so the API cannot read or
write arbitrary memory. Entries are static and stay valid for the life of the
program. Their fields are for inspection.

The public validators `alltrax_validate_var_value()` and
`alltrax_validate_var_string()` require the same library entries and reject
NULL or copied definitions with `ALLTRAX_ERR_INVALID_ARG`. They validate
values without device I/O; write permissions are checked by the write functions.

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

`alltrax_read_factory_vars()` takes the same arguments and reads the factory
values of user settings: each entry must lie in the user settings page and is
read from the same offset in the factory settings page. Other entries return
`ALLTRAX_ERR_INVALID_ARG` before device I/O.

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

### Raw memory reads

```c
#define ALLTRAX_READ_MEMORY_MAX 2048
bool alltrax_memory_readable(uint32_t address, size_t length);
alltrax_error alltrax_read_memory(alltrax_controller* ctrl, uint32_t address,
    size_t length, uint8_t* out);
```

For debugging. `alltrax_memory_readable()` performs no I/O and is true when
`length` bytes (1 to `ALLTRAX_READ_MEMORY_MAX`) at `address` lie within RAM
0x20000000-0x2000FFFF, FLASH 0x08000000-0x0801FFFF, or the backup registers
0x40006C00-0x40006C13. `alltrax_read_memory()` reads such a span into `out`,
subject to the normal firmware read bounds, and returns
`ALLTRAX_ERR_INVALID_ARG` before any I/O for any other span or a NULL `out`.
There is no raw write.

### Writing variables

```c
alltrax_error alltrax_write_vars(alltrax_controller* ctrl,
    const alltrax_var_def** vars, const double* values, size_t count,
    const alltrax_write_opts* opts);
```

Takes arrays of variable definitions and display-scale values. RAM and FLASH
variables can be mixed freely — the library separates them internally and uses
a single CAL/RUN bracket wrapping all writes. Values are automatically
converted from display units to raw device format.

Variable and curve writes attempt RUN restoration whenever CAL may have been
sent, including when the CAL transfer or acknowledgement fails. Failures before
CAL is attempted and a definite read-only rejection do not trigger RUN
cleanup. If the operation and restoration both fail, the original error
code is returned and `alltrax_last_error_detail()` includes both failures
(abbreviated to fit its buffer). If only restoration fails, its error is
returned explicitly as a RUN restoration failure.
If the controller starts outside RUN, the library writes in its existing mode.

The library enforces each definition's `writable` permission. A read-only
variable anywhere in a batch returns `ALLTRAX_ERR_INVALID_ARG` before device
I/O, rejecting the entire batch. Write options cannot bypass this check.
The CLI's `write` command relies on this library check and reports its diagnostic.
It opens the controller before reporting permission errors.

Numeric values must be finite. Integer encoding uses
`raw = round(value / scale) + offset`, rounding halfway values away from zero,
and requires the result to fit both the storage type and the variable's bounds.
Validation and encoding use the same checked conversion. Finite boolean values
normalize zero to false and nonzero to true. The CLI also rejects empty numbers
and numeric parsing overflow or underflow.

`alltrax_write_vars()` takes numeric and boolean values only; a string variable
returns `ALLTRAX_ERR_INVALID_ARG` before device I/O. Use
`alltrax_write_assignments()` for strings.

Pass `NULL` for `opts` to use the defaults. Pass an `alltrax_write_opts`
struct to allow unvalidated firmware or to collect a report:

| Field | Effect |
|-------|--------|
| `skip_fw_check` | Allow writes on unvalidated firmware within V0.001-V5.999 (CLI `--allow-unvalidated-firmware`) |
| `report` | Optional `alltrax_write_report*`, cleared and then filled with the actions the write performed: `factory_restored`, `autocal_cleared`, `date_stamped`, `reset_sent`. Each flag is set once its action succeeded, so a failed write still reports what ran. |

Each variable or curve may appear once per batch; repeated or overlapping
targets return `ALLTRAX_ERR_INVALID_ARG` before device I/O. The library
checks values against the catalog only. Limits that depend on the
controller's rating, hardware or other settings (rated-voltage ranges, voltage
order, the throttle types the hardware allows) are applied by the CLI's
[value rules](#value-rules).

After a successful write, the library stamps FieldMap_Date when the batch
contains `F_Table_Name`, clears throttle auto-calibration when it contains
`Throttle_Type` or `HPD`, and sends one reset after any settings-page write
except a batch of only the linearization curve and field-map data. A factory
recovery always resets.

**RAM variables** (`is_flash=false`) take effect immediately but are not
persistent across power cycles.

**FLASH variables** (`is_flash=true`) are persistent. The library performs a
full page cycle: read the 2KB settings page, patch in the new values, erase
the page, write it back, and verify. Multiple FLASH variables are batched into
a single page cycle, plus a separate cycle if factory recovery is needed.
Requires `allow_writes=true` and the write firmware policy.

When both RAM and FLASH variables are written together, all writes share a
single CAL/RUN bracket — saving 4 USB packets compared to separate calls.

Example:

```c
const alltrax_var_def* vars[] = {
    alltrax_find_var("LoBat_Vlim"),   /* FLASH */
    alltrax_find_var("HiBat_Vlim"),   /* FLASH */
    alltrax_find_var("Fan_On"),       /* RAM */
};
double values[] = { 28.1, 61.0, 1.0 };
alltrax_write_vars(ctrl, vars, values, 3, NULL);
```

### Batch writes with strings and curves

```c
typedef struct {
    const alltrax_var_def* var;            /* a variable, or */
    double number;                         /*   numeric/bool value */
    const char* text;                      /*   string value */
    const alltrax_curve_def* curve;        /* a user curve */
    const alltrax_curve_data* curve_data;
} alltrax_assignment;

alltrax_error alltrax_write_assignments(alltrax_controller* ctrl,
    const alltrax_assignment* items, size_t count,
    const alltrax_write_opts* opts);
alltrax_error alltrax_validate_var_string(const alltrax_var_def* var,
    const char* text);
```

`alltrax_write_assignments()` writes numeric, boolean, and string variables
and user curves in one call. Each item sets exactly one of `var` or `curve`.
String variables require `text`; other variables require `text == NULL`. Every
item is validated before device I/O, and an invalid item rejects the whole
batch. All writes share one CAL/RUN bracket, and all FLASH items share one
settings page cycle (one erase). The limits are 16 RAM variables and 96 FLASH
patches (a curve uses two). `alltrax_write_vars()` and `alltrax_write_curve()`
are single-kind wrappers around it.

```c
const alltrax_curve_def* lin = alltrax_find_curve("linearization");
alltrax_curve_data curve;                 /* filled by the caller */
alltrax_assignment items[] = {
    { .var = alltrax_find_var("Throttle_Type_Name"), .text = "0-5K 2 Wire" },
    { .var = alltrax_find_var("HPD"), .number = 1 },
    { .curve = lin, .curve_data = &curve },
};
alltrax_write_assignments(ctrl, items, 3, NULL);  /* one erase */
```

### Curve tables

Curve tables are 16-point X/Y arrays stored in FLASH, used for throttle
linearization, speed/torque limits, and field weakening. They have a separate
API from the scalar variable system because each curve is a pair of 16-element
arrays rather than a single value.

**Curve definitions:**

```c
const alltrax_curve_def* alltrax_find_curve(const char* name);
size_t                   alltrax_curve_count(void);
const alltrax_curve_def* alltrax_curve_by_index(size_t index);
```

`alltrax_find_curve()` looks up a curve by name (e.g. `"linearization"`),
returning `NULL` if not found. `alltrax_curve_count()` returns the total number
of defined curves (5). `alltrax_curve_by_index()` returns a curve by its index
(0-4), or `NULL` if out of range.

**`alltrax_curve_def` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `name` | `const char*` | Curve name (e.g. `"linearization"`) |
| `description` | `const char*` | Human-readable description |
| `x_address` | `uint32_t` | User X array FLASH address |
| `y_address` | `uint32_t` | User Y array FLASH address |
| `factory_x_address` | `uint32_t` | Factory X array FLASH address |
| `factory_y_address` | `uint32_t` | Factory Y array FLASH address |
| `x_scale` | `double` | X display scaling: `display = raw * x_scale` |
| `y_scale` | `double` | Y display scaling: `display = raw * y_scale` |
| `x_unit` | `const char*` | X axis unit (e.g. `"%"`, `"A"`) |
| `y_unit` | `const char*` | Y axis unit |
| `x_raw_max` | `int16_t` | Maximum raw X value (4095 for throttle, 7500 for field) |
| `y_raw_max` | `int16_t` | Maximum raw Y value (4095 for throttle, 5000 for field) |

**Reading curves:**

```c
alltrax_error alltrax_read_curve(alltrax_controller* ctrl,
    const alltrax_curve_def* def, alltrax_curve_data* out);
alltrax_error alltrax_read_curve_factory(alltrax_controller* ctrl,
    const alltrax_curve_def* def, alltrax_curve_data* out);
```

`alltrax_read_curve()` reads the user (writable) copy of a curve from the
device. `alltrax_read_curve_factory()` reads the factory (read-only) copy.
Both populate an `alltrax_curve_data` struct with scaled display values.

**`alltrax_curve_data` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `def` | `const alltrax_curve_def*` | Pointer to the curve definition |
| `x[16]` | `double` | X values in display units |
| `y[16]` | `double` | Y values in display units |

Example — read and print a curve:

```c
const alltrax_curve_def* def = alltrax_find_curve("linearization");
alltrax_curve_data curve;
alltrax_read_curve(ctrl, def, &curve);

for (int i = 0; i < ALLTRAX_CURVE_POINTS; i++)
    printf("  %6.2f %s -> %6.2f %s\n",
           curve.x[i], def->x_unit, curve.y[i], def->y_unit);
```

**Writing curves:**

```c
alltrax_error alltrax_validate_curve(const alltrax_curve_def* def,
    const alltrax_curve_data* data);
alltrax_error alltrax_write_curve(alltrax_controller* ctrl,
    const alltrax_curve_def* def, const alltrax_curve_data* data,
    const alltrax_write_opts* opts);
```

Writes both X and Y arrays in a single FLASH page cycle (read page, patch both
arrays, erase, write, verify). Uses the same `alltrax_write_opts` as
`alltrax_write_vars()` — pass `NULL` for safe defaults. Requires
`allow_writes=true`.

`alltrax_validate_curve()` checks all 16 points without device I/O. As with
variables, the curve must be a library entry, from `alltrax_find_curve()` or
`alltrax_curve_by_index()`; curve reads and writes require the same. Each
finite point must round into `[0, x_raw_max]` or `[0, y_raw_max]` using the
existing rounding policy. Zero padding and repeated points are accepted; no
ordering constraint is imposed. The writer enforces this same validation
before any device request, regardless of write options, and returns
`ALLTRAX_ERR_INVALID_ARG` for invalid data. CLI and config inputs use the same
library validation.

Example — copy factory curve to user:

```c
const alltrax_curve_def* def = alltrax_find_curve("linearization");
alltrax_curve_data factory;
alltrax_read_curve_factory(ctrl, def, &factory);
alltrax_write_curve(ctrl, def, &factory, NULL);
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
| `error_history[17]` | `uint16_t` | Milliseconds left of a 10-second window after each error: 9999 while the error is active, then counting down by 1 per millisecond to 0 once it clears |
| `battery_volts` | `double` | Battery voltage (V) |
| `output_amps` | `double` | Output current (A) |
| `battery_amps` | `double` | Battery current (A) |
| `field_amps` | `double` | Field current (A) |
| `temp_c` | `double` | Controller temperature (C) |
| `state_of_charge` | `double` | Battery state of charge (%) |
| `speed_rpm` | `int16_t` | Motor speed (RPM); meaningful only when `alltrax_info.speed_sensor` is true |
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
| `bad_vars_code` | `uint16_t` | Settings corruption code |
| `user1_input` | `bool` | User 1 input state |
| `user2_input` | `bool` | User 2 input state |
| `user3_input` | `bool` | User 3 input state |
| `user_profile_num` | `int16_t` | Selected profile: 0 Normal, 1 User 1, 2 User 2 (see below) |
| `user1_input_percent` | `int16_t` | User 1 analog input, 0-100 %; negative means disconnected (see below) |
| `user2_input_percent` | `int16_t` | User 2 analog input, 0-100 %; negative means disconnected (see below) |

The profile and analog input fields require firmware V1.005+
(`alltrax_has_feature(info, ALLTRAX_FEAT_USER_PROFILES)`) and are meaningful
only for the configuration selected by the `UserInputs_State` setting:
state 1 uses `user_profile_num` and `user1_input_percent`; state 2 uses
`user1_input_percent` and `user2_input_percent`; state 0 (switches) uses the
`user1_input`/`user2_input` booleans.

```c
const char* alltrax_error_flag_name(int index);
```

Returns the name of error flag at `index` (0-16), e.g. `"Overcurrent"`,
`"Overtemperature"`. Returns `NULL` for invalid indices.

### Device reset

```c
alltrax_error alltrax_reset_device(alltrax_controller* ctrl,
    const alltrax_write_opts* opts);
alltrax_error alltrax_clear_autocal(alltrax_controller* ctrl,
    const alltrax_write_opts* opts);
```

Sends a reset command. Reset follows the write firmware policy; `opts` may be
`NULL`, and only `skip_fw_check` is used. The controller reboots immediately (USB disconnects).
The handle can no longer communicate with it; close it after an optional
`alltrax_wait_for_reconnect()`. Settings writes reset automatically (see
`alltrax_write_assignments()`); a separate reset reloads settings at another
time. `alltrax_clear_autocal()` clears
the throttle backup registers before issuing the reset. If the reset fails
after the clear, the learned values may already be gone. Both calls read the
controller's model and serial number, send the reset report, and return.

```c
alltrax_error alltrax_wait_for_reconnect(alltrax_controller* ctrl,
    unsigned timeout_ms);
```

After a reset sent through the handle (including one sent by a write),
`alltrax_wait_for_reconnect()` waits up to `timeout_ms` for the same
controller, matched by model and serial number, to answer read-only requests
again. It returns `ALLTRAX_OK`, `ALLTRAX_ERR_TIMEOUT`, or
`ALLTRAX_ERR_INVALID_ARG` if no reset was sent. The handle itself stays stale:
close it and open a new one. The CLI waits by default.

```c
alltrax_error alltrax_list_devices(const char* path, alltrax_device_visitor visit,
    void* user, size_t* count);
```

`alltrax_list_devices()` probes enumerated devices with read-only requests.
It calls the supplied visitor with each responsive controller's path, model,
and serial number, and stores the number found in `*count`. The path and
model strings are valid during the callback.

One process at a time talks to controllers. `alltrax_open()` and
`alltrax_list_devices()` take an exclusive lock on `/tmp/alltrax.lock`
before any USB access and return `ALLTRAX_ERR_BUSY` while another handle or
process holds it; `alltrax_close()` releases it. The lock covers this
library only, not other programs such as the Windows Toolkit.

## Variable Reference

All controller settings and status values are accessed through named variables.
Variables are organized into groups and stored in either **FLASH** (persistent
across power cycles) or **RAM** (live state, lost on power cycle).

Factory defaults vary by controller model and electrical ratings. Use
`get --factory` with the user setting name, or read an explicit factory
entry such as `F_Reverse_Field_Weaken_Percent`.

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

The CLI's [value rules](#value-rules) limit these settings to 16.0–62.0 V on
48 V controllers and 20.0–90.0 V on 72 V controllers, and refuse them on other
ratings. They also require the analog threshold to be at least 1 V below the
low cutoff, the high cutoff at least 1 V above the low cutoff, and
`BMS_Missing_HiBat_Vlim` at least 1 V above the low cutoff and at or below the
high cutoff. Settings not being written are read from the controller.

#### User Profiles (Normal, User 1, User 2)

Three user profiles with identical structure. The **Normal** profile uses the
`N_` prefix, **User 1** uses `U1_`, and **User 2** uses `U2_`. The active
profile is selected by the user inputs.

Profile current is expressed as a percentage of the controller's rated
current. For a 400 A controller, 100% is 400 A. `write` accepts a bare
number as a signed percentage or a current magnitude with an `A` suffix.
Regen and plug braking use negative percentages and positive amp magnitudes.
`get` shows both units when the rating is known.

The `_Max` variables set the upper end of the adjustable range in analog
modes. Older settings can store -1 as a placeholder on some maximum fields.

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
| `N_Hunting_Buggy` | Hunting Buggy Mode | bool | |
| `N_Max_Arm_Regen_Amps_Max` | Max regen amps user-adjustable ceiling | % | -100 – 0 |
| `N_Speed_Limit_Max` | Speed limit user-adjustable ceiling (-1 = disabled) | RPM | -1 – 8000 |
| `N_Forward_Speed` | Max forward speed when speed limiting is off | % | 0 – 100 |
| `N_Forward_Speed_Max` | Forward speed user-adjustable ceiling (-1 = disabled) | % | -1 – 100 |
| `N_Throttle_Rate` | Throttle acceleration rate | MU | 0.5 – 16383.5 |
| `N_Throttle_Rate_Max` | Throttle rate user-adjustable ceiling (-1 = disabled) | MU | -0.5 – 16383.5 |

#### Throttle

Throttle type selection, calibration values, and related settings.

`Throttle_Type` selects the throttle hardware:

Assignments must be integral values from 0 through 11; fractions and undefined
types are rejected. The CLI's [value rules](#value-rules) also require each
nonzero type to be allowed by the controller's hardware mask. On older
hardware without a programmed mask, the fallback allows every type except
EZGO ITS. Type 0 (None) is always allowed and does not require a mask read.

Writing `Throttle_Type` or `HPD` clears the learned throttle calibration.

| Value | Type |
|-------|------|
| 0 | None |
| 1 | 0-5k ohm (2-wire) |
| 2 | 5k-0 ohm (2-wire, reversed) |
| 3 | 0-5V |
| 4 | EZGO ITS |
| 5 | 0-5K 3 Wire (includes Yamaha and Club Car labels) |
| 6 | Taylor Dunn |
| 7 | 5K-0 3 Wire (includes Club Car labels) |
| 8 | Alltrax Digital |
| 9 | Pump |
| 10 | USB |
| 11 | Absolute |

| Variable | Description | Unit | Bounds |
|----------|-------------|------|--------|
| `Throttle_Type` | Throttle hardware type (see table above) | | 0–11, integer |
| `HPD` | High Pedal Disable — block drive if throttle is pressed at key-on | bool | |
| `Throttle_Type_Name` | Name of the selected throttle (16 chars) | | |
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

#### Tach

Speed sensor settings and their read-only factory copies.

| Variable | Description | Unit |
|----------|-------------|------|
| `Speed_Limit_On` | Enable speed limiting (uses `N_Speed_Limit` etc.) | bool |
| `Tach_4_8` | Speed sensor type: No = 4-pole, Yes = 8-pole | bool |
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

Writing `F_Table_Name` updates the field-map date.

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
| `BKP_Throttle_Minima` | Learned minimum throttle, read-only backup register | |
| `BKP_Throttle_Maxima` | Learned maximum throttle, read-only backup register | |

#### Controls

RAM control variables. Writable ones take effect immediately but do not
persist across power cycles.

| Variable | Description | Unit |
|----------|-------------|------|
| `RunMode` | Controller mode: 0 = RUN (normal), 255 = CAL (settings mode). Read-only; the library sets it around writes. | |
| `Fan_On` | Fan test trigger. Writing `1` starts a timed fan test (~10–30 seconds); the firmware stops it automatically. This is a momentary command, not a toggle — reading it always returns 0. | bool |

## Supported hardware

Tested on:

- **Alltrax XCT48400-DCS** (USB PID `0x0002`, firmware V5.005)

Should work with other XCT-series controllers sharing the same USB Product ID.

### Device detection

The library auto-detects the controller family from the USB Product ID and
model string prefix:

| USB PID | Model Prefix | Type | Supported |
|---------|-------------|------|-----------|
| `0x0002` | XCT, SRX, NCT | XCT | Yes |
| `0x0001` | SPM, SPB | SPM | Rejected |
| `0x0001` | SR* (not SRX) | SR | Rejected |
| `0x0001` | BMS | BMS | Rejected |
| `0x0001` | BMS2 | BMS2 | Rejected |

PID 1 devices are rejected before opening, including for `alltrax info`.
Their checksum and identity/revision handling require a separate implementation.
PID 2 devices with unrecognized model prefixes are closed after the read-only
model check. Every public device operation requires a validated XCT-family
identity, independently of firmware overrides.

Have a PID `0x0001` device (SPM, SPB, SR, BMS, or BMS2)? Contributions toward
support are welcome. We currently lack hardware to validate these devices.

## How it works

Alltrax controllers expose a USB HID interface. The host reads and writes
directly to ARM memory addresses on the STM32 microcontroller inside the
controller — there is no abstraction layer. Settings live in FLASH at fixed
addresses determined by the USB Product ID.

## License

LGPL-3.0-or-later. See [LICENSE](LICENSE).
