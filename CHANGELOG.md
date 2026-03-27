# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] — 2026-03-26

### Added
- **Sidebar panel** — port selection, serial configuration, start/stop, and
  active session list are now displayed in a dedicated LogSquirl sidebar tab
  (replaces the standalone QDialog). Sessions show rotate (↻) and stop (■)
  buttons.
- **Log directory** — configurable log save path with automatic filename
  generation (`YYYY-MM-dd_HHmmss_<portName>.log`). The path is persisted
  across sessions.

### Changed
- Plugin UI type now uses `register_sidebar_tab()` instead of
  `register_menu_action()` for the main interface.
- Session list shows only the port name (no line count).

### Fixed
- Session list no longer displays redundant line counts next to the port name.

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

[Unreleased]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/releases/tag/v0.1.0
