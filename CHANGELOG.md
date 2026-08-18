# Changelog

## 0.3.0

- Changed the project license to PolyForm Noncommercial License 1.0.0.
- Added the copyright required notice for Hohenstein256.
- Uses `-O2` as the default optimization level.
- Makes link-time optimization optional with `make LTO=1`.
- Reduced live-mode allocations and string formatting in the sensor sampling path.
- Added clean `Ctrl+C` and termination handling with a partial final summary.
- Added `make check` smoke tests.
- Improved install path handling with `PREFIX`, `BINDIR`, and `DESTDIR`.
- Added a minimal `.gitignore` for repository use.
- Reworked the README for a public GitHub repository.
- Documented tested and expected Linux compatibility.
- Added a Linux x86_64 release workflow and static release build support.

## 0.2.0

- Added a single-line live display.
- Added Wh integration and optional electricity cost extrapolation.
- Reused sysfs file descriptors during live sampling.
- Switched sensor discovery to once per live session.

## 0.1.1

- Added final live summaries.
- Improved AMD PPT labeling.
- Filtered zero-value non-battery power supplies from live output.

## 0.1.0

- Initial release.
