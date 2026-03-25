# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] — 2026-03-25

### Added

- Initial release of the LogSquirl Serial Monitor plugin.
- Serial port discovery via `QSerialPortInfo` with Bluetooth filtering.
- Full serial parameter configuration: baud rate, data bits, stop bits,
  parity, and flow control.
- Optional `[YYYY-MM-DD HH:mm:ss.zzz]` timestamp prefix per received line.
- Multi-port simultaneous capture — each port opens in its own LogSquirl tab.
- Save-to-file option for persistent log storage.
- Persistent temp files — captured output remains visible after stopping.
- Configurable default baud rate via plugin configuration dialog.
- Unit tests with Catch2 v2 (BDD style).
- CI build workflow for Linux, macOS, and Windows.
- CI release workflow with per-platform ZIP artifacts and checksums.

[0.1.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/releases/tag/v0.1.0
