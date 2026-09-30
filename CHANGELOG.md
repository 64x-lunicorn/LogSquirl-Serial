# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- Log messages and notifications reach LogSquirl as UTF-8, so non-ASCII
  port names and paths are no longer garbled on systems whose local
  8-bit encoding is not UTF-8.
- Lines ending in `\r\n` no longer keep a stray carriage return, and a
  lone `\r` now ends a line. A stream that never ends its lines (binary
  data, a progress display) is written out every 64 KiB instead of
  growing the read buffer without bound.
- A session whose port cannot be opened is no longer listed as active:
  the error is shown once, no tab is opened, and no empty log file is left
  behind.
- Temporary log files are now actually removed on shutdown (0.4.0's temp
  file cleanup never took effect): each stopped session reported back that
  it had ended, and handling that preserved its file.
- Unplugging a device ends its session: it is removed from the list and
  the user is told the device was disconnected. Before, the session stayed
  listed as active, capturing nothing.
- The sidebar no longer sends a command to some other running capture
  when the selected port has none; it says so instead. Its send controls
  are only enabled for a selected port with a running capture, as in the
  dialog.
- The Serial Monitor dialog (Plugins menu) stays on top of LogSquirl's
  window, and no longer keeps LogSquirl running when it is open while the
  main window is closed.
- Deleting the dialog with sessions still running no longer calls back
  into the half-destroyed dialog.
- Starting, stopping and rotating never truncate an existing log file.
  A save path is appended to, so Stop and Start with the dialog's fixed
  save path keep the earlier capture; generated file names get a `_2`,
  `_3`, … suffix when a file of that name exists, so a rotation within the
  same second as the start no longer wipes the capture it rotates away
  from.
- A rotation that cannot create its new file no longer leaves the session
  running with its log file closed, which silently dropped all further
  output: the capture continues in the old file and the error is shown.
- A second session is refused instead of writing into the save path of
  one that is still running.
- A port given as a device path (`/dev/ttyUSB0`) gets a valid temporary
  file name: the port name is sanitised, as it already was for files in
  the log directory.

## [0.5.0] — 2026-06-22

### Added
- **Serial TX commands** — added command input + send button in both the
  sidebar panel and the port dialog, with configurable line endings
  (`CRLF`, `LF`, `CR`, `None`).
- **TX logging** — sent commands are now written into the active log stream
  and marked with `[TX]` for visibility in LogSquirl tabs.

### Changed
- **Serial port mode** — sessions now open ports in read/write mode to support
  interactive command transmission while receiving data.

## [0.4.0] — 2026-04-02

### Added
- **Plugin icon** — added `icon` field to `plugin.json` for display in the
  host Plugin Management dialog.
- **Decentralized registry** — added `releases.json` with per-platform download
  URLs and SHA-256 checksums for all releases.
- **Temp file cleanup** — temporary log files are removed on shutdown.
- **Default log directory** — logs are saved to a configurable default directory.

## [0.3.0] — 2026-03-27

### Added
- **Plugin registry** — README now links to the
  [LogSquirl-Plugins](https://github.com/64x-lunicorn/LogSquirl-Plugins) registry
  with Mermaid diagram showing the install flow.

### Fixed
- **macOS CI**: Bundle `QtSerialPort.framework` alongside the plugin in the
  release ZIP so the host app does not need to ship it.
- **macOS CI**: Strip CI-specific rpaths and add portable `@loader_path` entries
  so the plugin resolves frameworks from the host app bundle.
- **macOS CI**: Handle flat Qt6 framework layout (`QtSerialPort.framework/QtSerialPort`)
  used by newer aqtinstall versions.
- **macOS CI**: Extract Qt6 lib path from CMake cache instead of relying on the
  `Qt6_DIR` shell environment variable which is not always exported.

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

[Unreleased]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/releases/tag/v0.1.0
