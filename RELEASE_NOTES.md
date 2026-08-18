# PowerCalc 0.3.0

PowerCalc is a lightweight Linux power telemetry and electricity cost utility by Hohenstein256.

## Highlights

- Live whole-system laptop power estimates from battery telemetry.
- Intel RAPL and other powercap energy counters.
- hwmon power telemetry, including conservative AMD SoC/PPT labeling.
- Single-line live monitoring with a final summary instead of terminal spam.
- Session energy integration in Wh.
- Optional electricity cost extrapolation.
- Cached sysfs file descriptors and one-time sensor discovery for low sampling overhead.
- Clean `Ctrl+C` handling with a partial summary.

## Linux binary

The attached release archive contains a statically linked **Linux x86_64** binary.

Verified compatibility:

- Debian 13 x86_64: build and smoke tests pass.
- Zorin OS x86_64: battery, AMD PPT, and Intel RAPL telemetry have been observed working on real hardware.

Other x86_64 Linux distributions are expected to work when the kernel exposes the standard `power_supply`, `hwmon`, and `powercap` sysfs interfaces. Sensor availability varies by machine. ARM and other architectures are not part of this release.

PowerCalc is not a wall wattmeter. Battery telemetry is a useful whole-system estimate on a discharging laptop, while desktop total AC wall power generally requires hardware capable of exposing that measurement.

## License

PolyForm Noncommercial License 1.0.0. See `LICENSE` for the complete terms.
