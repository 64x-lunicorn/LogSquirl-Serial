# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Saved port configurations, one tile each in the sidebar.** The dialog
  now saves a port with its baud rate, data bits, stop bits, parity, flow
  control, timestamps and log directory under a name, in `serial.ini` in the
  plugin's configuration directory, so it is there again after a restart.
  The Serial sidebar tab shows a tile for each saved configuration, with its
  port, its frame such as `115200 8N1` and its flow control, and Start /
  Stop, Edit… and Delete; *New configuration…* opens an empty dialog, and a
  new configuration proposes the defaults of Configure and the log directory
  saved last. A configuration edited in the dialog and saved under another
  name is renamed, tile and all, unless its capture runs or the name is
  taken. Tiles on different ports capture at the same time; a tile
  whose port another one captures says so, and its Start is refused with a
  notification naming the configuration that holds the port. The sidebar's
  inline form and its list of active sessions are gone, as is the dialog's
  Start / Stop: captures are started from their tiles (#50).
- **A running tile shows what its capture does, with Send and Rotate.**
  While it runs, a tile shows, updated every second, the lines per second
  and how long it has run, the lines written and the bytes received, the
  size of the current file, the size at which the next file starts and how
  many files it has started so far. Below them, a command field with its
  line ending (CRLF, LF, CR or none) and **➤ Send** send a command to the
  tile's port, and **↻ Rotate** goes on in a new log file and tab: both are
  back after the sidebar's inline form went. The line ending chosen and a
  command typed but not sent yet stay while other configurations are saved
  or deleted. A stopped tile shows its configuration only (#51).
- **A log file is rotated at a size you choose.** When a capture's log file
  reaches 1 GiB, the capture goes on in a new file, opened in a new tab, and
  a notification says why; a line being received is kept whole for the new
  file. *Start a new log file at* in Configure sets the size, or turns the
  limit off with *Never*. When the new file cannot be created, the capture
  stays in the old one and the rotation is tried again once it has grown by
  the limit once more. Files in the log directory are still never removed
  (#33).

### Changed
- **The icon matches the other LogSquirl plugins.** The connector is centred
  and drawn with the same line width as the other plugin icons (#44).
- **Built against the plugin API of LogSquirl 26.12.** LogSquirl 26.11 and
  later start the plugin through `logsquirl_plugin_init_ex()` and tell it
  which of their functions it may use; LogSquirl 26.10 starts it as before,
  so the plugin still runs there.

### Fixed
- **An error when a capture ends, fails or reaches its size limit no longer
  ends LogSquirl.** What a running capture reports — that it ended, an
  error, or that its log file reached the size limit and goes on in a new
  one — is handled in LogSquirl's event loop; an exception there, such as
  running out of memory, is now logged instead of ending LogSquirl. So is
  one when a tile's Edit… opens the dialog (#53).
- **An error in the plugin no longer ends LogSquirl.** An exception in the
  plugin's start, shutdown, Configure or its menu entry is logged instead
  of escaping into LogSquirl; a start that fails is undone, the sidebar tab
  unregistered before its widget goes.
- **A log file that cannot be written stops the capture.** When the disk is
  full, the log directory's drive goes away or the file cannot be written
  for another reason, the session stops and says once which file could not
  be written, instead of showing a running capture while every line is
  lost (#32).
- **A save path or log directory is a full path.** A leading `~` is your
  home directory; a relative path is refused with a message instead of
  being created in LogSquirl's working directory (`/` when it was started
  from the Finder or the Dock), so the tab always opens the file the
  capture writes (#35).
- **Only a regular file is written to.** A save path that is a FIFO, a
  device or a symbolic link is refused at once with a message, so it can
  no longer freeze LogSquirl waiting for a reader, nor send the capture
  elsewhere. On Linux and macOS a log file the plugin creates is readable
  by you only (#37).
- **Any error of an open port ends its session, with one notification.**
  Some drivers report a pulled adapter or a failure of their own as a read
  or unknown error rather than as a lost device; the session then kept
  running and showed a new notification on every pass of the event loop.
  Now every such error stops the capture and is shown once (#34).
- **Binary bytes from the port can no longer change how LogSquirl decodes
  the log.** NUL padding, colour codes and the other control characters but
  tab are written as a visible escape such as `\x00` or `\x1B`, two
  upper-case hex digits, instead of raw, where they could make LogSquirl
  take the whole file for UTF-16 or another encoding. A line forced out
  after 64 KiB without a line end no longer cuts a UTF-8 character in two
  (#38).
- **A sent command no longer splits a received line.** When the device had
  sent part of a line, the `[TX]` line was written before it and the two
  parts joined below, under a later timestamp. The part received so far is
  now written first, as a line of its own (#39).
- **Stopping a session no longer waits for unsent output.** A command the
  device had not taken yet, held back by hardware flow control or a pulled
  cable, could keep Stop or LogSquirl's shutdown waiting up to 30 seconds
  while the port closed; it is now discarded first (#40).
- **Configure says when the defaults could not be saved.** The dialog
  closed as if it had saved them when the settings file could not be
  written; it now tells you. A LogSquirl that gives the plugin no
  configuration directory gets a message instead of the dialog, which
  would have tried to write `/serial.ini` at the root of the filesystem
  (#41).
- **The empty log directory says where captures really go.** It read "logs
  saved to temp directory", but captures are saved, and kept, in `logs` in
  the plugin's configuration directory; the field now shows that directory
  (#36).
- **The sidebar lists the ports that really capture.** When one session
  ended and another started within the same second, the list kept the
  ended port, with buttons that did nothing, and left out the new one (#45).
- **A log directory that cannot be saved is reported.** When the
  configuration directory is read-only or full, the sidebar says so instead
  of forgetting the typed log directory on the next start without a word (#46).

## [0.6.0] — 2026-09-30

### Changed
- **Requires LogSquirl 26.10.0 or later.** This release is built with the Qt of
  LogSquirl 26.10.0 (Qt 6.11.3); an older LogSquirl cannot load it.

### Fixed
- Log messages and notifications reach LogSquirl as UTF-8, so non-ASCII
  port names and paths are no longer garbled on systems whose local
  8-bit encoding is not UTF-8.
- Lines ending in `\r\n` no longer keep a stray carriage return, and a
  lone `\r` now ends a line, as soon as it arrives. A stream that never ends its lines (binary
  data, a progress display) is written out every 64 KiB instead of
  growing the read buffer without bound.
- A session whose port cannot be opened is no longer listed as active:
  the error is shown once, no tab is opened, and no empty log file is left
  behind.
- Temporary log files are now removed when LogSquirl quits: those of the
  running sessions and of every session that was stopped, ended or
  rotated away before, whose tabs close with LogSquirl. When the plugin is
  disabled or updated while LogSquirl keeps running, its sessions are
  stopped but every file is kept, since open tabs may still show them.
  0.4.0's cleanup never took effect: each session stopped at shutdown
  reported back that it had ended, and handling that preserved its file,
  while the files of earlier and rotated sessions were never removed at
  all. Save paths and files in the log directory are never removed.
- Temporary log files are now also removed when LogSquirl quits after the
  plugin was disabled and enabled again, or updated, while it kept running.
  The new plugin did not know the files the earlier one had kept for its
  tabs, so they stayed for good. Their directories are now named
  `logsquirl-serial-<pid>-XXXXXX` after the LogSquirl process, and at quit
  the plugin removes every one with its own process ID. Directories left
  behind by a LogSquirl that no longer runs, e.g. after a crash, are removed
  when the plugin starts; those of a running LogSquirl, links and, on Unix,
  other users' directories are left alone.
- Starting or stopping a capture no longer enumerates the serial ports,
  which blocks the UI, up to twice per click (once for the dialog, once
  for the sidebar). The dialog and sidebar now share one port list, which
  is rescanned at startup, on Refresh and when a session ends by itself.
- Unplugging a device ends its session: it is removed from the list and
  the user is told the device was disconnected. Before, the session stayed
  listed as active, capturing nothing.
- The sidebar no longer sends a command to some other running capture
  when the selected port has none; it says so instead. Its send controls
  are only enabled for a selected port with a running capture, as in the
  dialog.
- Plugins → Configure showed the timestamp default but offered no way to
  change it. It is now a proper dialog with the default baud rate and a
  "Prepend timestamp to each line" checkbox, and applies both to the open
  Serial panels right away.
- A default baud rate set in Plugins → Configure that is not one of the
  listed rates (e.g. 250000) was silently ignored. It is now added to the
  list and selected. The baud rate boxes also accept any positive rate
  typed as plain digits (in every locale, so "250.000" is not mistaken
  for a rate), and a session with an invalid rate is refused with a message.
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

[Unreleased]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.6.0...HEAD
[0.6.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/64x-lunicorn/LogSquirl-Serial/releases/tag/v0.1.0
