# PowerCalc 0.3.0

PowerCalc is a small Linux power telemetry and electricity cost utility written in a single C++20 source file.

**Author:** Hohenstein256  
**License:** PolyForm Noncommercial License 1.0.0  
**Platform:** Linux

## Features

- Reads laptop battery power from `/sys/class/power_supply`.
- Reads direct hardware power sensors from `/sys/class/hwmon`.
- Reads RAPL and other energy counters from `/sys/class/powercap` and hwmon.
- Uses a discharging battery as the preferred whole-system laptop estimate.
- Labels AMD PPT conservatively as SoC telemetry because it may include CPU power on an APU.
- Handles RAPL counter wrap when `max_energy_range_uj` is available.
- Reuses open sysfs file descriptors during live sampling.
- Discovers sensors once per live session instead of rescanning `/sys` every sample.
- Uses actual elapsed time for power and energy calculations.
- Integrates whole-system energy in Wh during a live session.
- Supports optional electricity cost extrapolation.
- Uses a single refreshing terminal line in live mode.
- Prints only the final summary when stdout is redirected or piped.
- Stops cleanly with `Ctrl+C` and keeps the partial summary.

PowerCalc does not add overlapping battery, CPU, GPU, package, core, or SoC readings together.

## Build

```sh
make
make check
```

The default build uses:

```text
-std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic
```

`-O2` is the default optimization level. Runtime sampling is kept lightweight by discovering sensors once and reusing open sysfs file descriptors.

Optional link-time optimization can be enabled when the compiler and linker support it:

```sh
make LTO=1
```

A static Linux binary can be built when the required static system libraries are available:

```sh
make STATIC=1
```

A C++20-capable compiler and Linux are required.


## Compatibility

PowerCalc is Linux-only.

The official `0.3.0` release binary targets **64-bit x86 Linux (x86_64)** and is built as a static ELF executable. It does not require a matching system C++ runtime.

Compatibility currently verified:

- Debian 13 x86_64: build and smoke tests pass with GCC and Clang.
- Zorin OS x86_64: live battery, AMD PPT, and Intel RAPL telemetry have been observed working on real hardware.

Other x86_64 Linux distributions are expected to work when the kernel exposes the standard sysfs interfaces used by PowerCalc:

- `/sys/class/power_supply`
- `/sys/class/hwmon`
- `/sys/class/powercap`

Sensor availability is hardware- and kernel-dependent. A system may run PowerCalc correctly while exposing only part of the telemetry. Desktop whole-system wall power usually cannot be measured unless suitable hardware telemetry exists.

ARM and other architectures are not included in the official `0.3.0` binary release. Building from source on those architectures is not currently claimed as tested.

## Usage

```sh
./powercalc scan
./powercalc live
./powercalc live 1 60
./powercalc live 1 60 0.23
./powercalc cost 80 0.23 6 30
./powercalc estimate 500 35 8 0.23
./powercalc --version
./powercalc --help
```

### Scan sensors

```sh
./powercalc scan
```

Shows power supplies, hwmon power sensors, and energy counters visible through Linux sysfs.

### Live monitoring

```sh
./powercalc live [interval_seconds] [sample_count] [price_per_kWh]
```

Example:

```sh
./powercalc live 1 120 0.23
```

This samples once per second for up to 120 samples and uses `0.23` as the electricity price per kWh.

On an interactive terminal, one status line is refreshed in place. Press `Ctrl+C` to end the session early and print the measurements collected so far.

The final summary includes whole-system average, minimum and maximum power, measured Wh, CPU RAPL data when available, AMD SoC/PPT data when available, and cost projections when a price is provided.

### Cost calculation

```sh
./powercalc cost <watts> <price_per_kWh> [hours_per_day=24] [days=365]
```

Example:

```sh
./powercalc cost 80 0.23 6 30
```

### Estimated load calculation

```sh
./powercalc estimate <rated_watts> <load_percent> <hours_per_day> <price_per_kWh> [days=365]
```

Example:

```sh
./powercalc estimate 500 35 8 0.23
```

Both decimal points and a single decimal comma are accepted for numeric arguments.

## Laptop measurement

For the best software-only whole-system estimate on a laptop, disconnect the charger and wait until the battery reports `Discharging`.

Battery telemetry represents DC power leaving the battery. It is usually more useful for whole-system laptop power than CPU or GPU telemetry, but it is still not the same measurement as AC wall power.

## Accuracy and limitations

PowerCalc is not a wall wattmeter.

On a desktop, Linux usually cannot know total AC wall power unless the hardware exposes suitable telemetry. PSU conversion losses and components without power sensors may be missing from software-visible measurements.

Sensor update rates also differ. A battery reading, an AMD PPT reading, and a RAPL reading may represent different time windows, so short-lived values should not be compared as if they were synchronized laboratory measurements.

## Install

```sh
sudo make install
```

The default install path is `/usr/local/bin/powercalc`.

A custom prefix can be used:

```sh
make PREFIX="$HOME/.local" install
```

Uninstall with:

```sh
sudo make uninstall
```

## License

PowerCalc is licensed under the PolyForm Noncommercial License 1.0.0.

Noncommercial use, modification, and distribution are permitted under the license terms. Commercial use is not granted by this license. See `LICENSE` for the full terms.
