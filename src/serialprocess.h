/*
 * Copyright (C) 2026 LogSquirl Contributors
 *
 * This file is part of logsquirl-serial.
 *
 * logsquirl-serial is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * logsquirl-serial is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with logsquirl-serial.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file serialprocess.h
 * @brief Serial port discovery and per-port session management.
 *
 * Each SerialProcess instance wraps a single QSerialPort connection.
 * Output is written to a temporary file (and optionally a user-chosen file)
 * so that LogSquirl can open it with follow/tail mode via the host API.
 *
 * THREAD MODEL
 * ────────────
 * SerialProcess objects live on the main thread.  QSerialPort signals
 * (readyRead, errorOccurred) are handled on the main thread's event loop.
 * File I/O is synchronous but fast (line-buffered writes).
 *
 * USAGE
 * ─────
 *   SerialConfig cfg;
 *   cfg.portName = "/dev/ttyUSB0";
 *   cfg.baudRate = 115200;
 *   auto* proc = new SerialProcess( cfg, "/optional/save.log", parent );
 *   if ( proc->start() )              // opens serial port
 *       qDebug() << proc->tempFilePath(); // LogSquirl opens this file
 *   proc->stop();                     // closes port
 */

#pragma once

#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace serial_monitor {

/**
 * Configuration for a serial port session.
 *
 * Taken from a saved configuration (profiles.h) and passed to the SerialProcess
 * constructor.  Sensible defaults match the most common embedded
 * device configuration: 115200 8N1, no flow control, timestamps on.
 */
/// Line ending appended to transmitted data.
enum class TxLineEnding {
    None, ///< No line ending appended.
    CR,   ///< Carriage return (\r).
    LF,   ///< Line feed (\n).
    CRLF  ///< Carriage return + line feed (\r\n).
};

/// Bytes in a MiB, the unit the size limit is set in.
constexpr qint64 kBytesPerMiB = 1024 * 1024;

/// Size in MiB at which a log file is rotated unless the user chose another.
constexpr int kDefaultMaxFileMiB = 1024;

struct SerialConfig {
    QString portName;
    qint32 baudRate = 115200;
    QSerialPort::DataBits dataBits = QSerialPort::Data8;
    QSerialPort::StopBits stopBits = QSerialPort::OneStop;
    QSerialPort::Parity parity = QSerialPort::NoParity;
    QSerialPort::FlowControl flowControl = QSerialPort::NoFlowControl;
    bool timestamps = true; ///< Prepend [YYYY-MM-DD HH:mm:ss.zzz] to each line.
    TxLineEnding txLineEnding = TxLineEnding::CRLF; ///< Line ending for sent data.
    /// Size at which the log file is rotated (SerialProcess::sizeLimitReached());
    /// 0 for no limit.
    qint64 maxFileBytes = kDefaultMaxFileMiB * kBytesPerMiB;
};

/**
 * Manages a single serial port session.
 *
 * Captures incoming data line-by-line and writes each line to:
 *   - The user-configured log directory file, when savePath is non-empty.
 *   - A temporary file inside a QTemporaryDir, when savePath is empty,
 *     named after the process (see tempdirs.h).
 *
 * When a savePath is provided the temporary directory is not used,
 * avoiding orphaned temp files that fill up disk space.
 *
 * The file path returned by tempFilePath() is what the host opens
 * via open_file().
 */
class SerialProcess : public QObject {
    Q_OBJECT

public:
    /**
     * Construct a serial session.
     *
     * @param config    Serial port configuration.
     * @param savePath  Optional path to a .log file for persistent saving.
     *                  Pass an empty string to disable saving.
     * @param parent    QObject parent for memory management.
     */
    explicit SerialProcess( const SerialConfig& config, const QString& savePath = {},
                            QObject* parent = nullptr );

    /** Closes the port without emitting any signal. */
    ~SerialProcess() override;

    // ── Static helpers ───────────────────────────────────────────────

    /**
     * Discover available serial ports.
     *
     * Uses QSerialPortInfo::availablePorts() and filters out Bluetooth
     * virtual ports and known non-serial entries.
     *
     * @return List of port names (e.g. "/dev/ttyUSB0", "COM3").
     */
    static QStringList discoverPorts();

    /**
     * Filter a list of QSerialPortInfo entries into usable port names.
     *
     * Extracted as a static helper so unit tests can exercise the
     * filtering logic without real hardware.
     *
     * @param ports  List of QSerialPortInfo from availablePorts().
     * @return Filtered list of port names.
     */
    static QStringList filterPorts( const QList<QSerialPortInfo>& ports );

    /// What rotateLog() does with the part of a line received so far.
    enum class PartialLine {
        WriteOut, ///< End it in the old file, as a line of its own.
        Keep      ///< Keep it for the new file, where the rest of it goes.
    };

    /// Longest line takeLines() waits for before forcing it out.
    static constexpr qsizetype kMaxLineLength = 64 * 1024;

    /**
     * Remove every complete line from the front of @p buffer and return
     * the lines, without their terminator.  "\n", "\r\n" and a lone "\r"
     * all end a line.  A CR ends its line at once, even as the last byte
     * read, and sets @p afterCr: an LF at the start of the next call is
     * then the second half of a CRLF pair and is skipped.  An incomplete
     * last line stays in the buffer until
     * more data arrives, unless it is longer than @p maxLineLength: then
     * it is returned as a line of its own, so that a stream that never
     * ends its lines (binary data, a progress display) cannot make the
     * buffer grow without bound.  Such a line ends before a UTF-8
     * character whose last bytes have not arrived yet; they stay in the
     * buffer, so that the character is not cut in two.
     *
     * Extracted as a static helper so unit tests can exercise the line
     * splitting without real hardware.
     *
     * @param buffer         Bytes read so far; complete lines are removed.
     * @param afterCr        In: whether the data before @p buffer ended with
     *                       a CR.  Out: whether @p buffer did.  Start with
     *                       false and keep it for the next call.
     * @param maxLineLength  Size above which an incomplete line is forced out.
     * @return The complete lines, in order.
     */
    static QList<QByteArray> takeLines( QByteArray& buffer, bool& afterCr,
                                        qsizetype maxLineLength = kMaxLineLength );

    /**
     * Return @p line with every C0 control character but tab (NUL, ESC,
     * the bell, …) written as a visible `\xNN` escape, two upper-case hex
     * digits.  The log file then holds text only, so that NUL padding or
     * garbage a device sends cannot make LogSquirl take the whole file for
     * UTF-16 or another encoding.  Other bytes, a backslash among them,
     * are left as they are.
     */
    static QByteArray escapeControls( const QByteArray& line );

    /**
     * Return a sensible default configuration (115200 8N1, no FC, timestamps on,
     * rotation at kDefaultMaxFileMiB), with the defaults saved through the
     * Configure dialog applied.
     */
    static SerialConfig defaultConfig();

    /**
     * Return a path for a new log file in @p dir, named
     * `<yyyy-MM-dd_HHmmss>_<port>.log`.  Characters of the port name that
     * are not valid in file names (the '/' of "/dev/ttyUSB0") are
     * replaced by '_'.  If that file exists, a number is appended
     * (`…_2.log`, `…_3.log`, …), so that two captures within the same
     * second never share a file.
     *
     * @param dir        Directory the file will be created in.
     * @param portName   Serial port name.
     * @param timestamp  Time the capture starts.
     * @return Absolute path of a file that does not exist yet.
     */
    static QString generateLogPath( const QString& dir, const QString& portName,
                                    const QDateTime& timestamp = QDateTime::currentDateTime() );

    /**
     * Return the plugin's config directory from the host API.
     * Falls back to a temp path if the plugin is not initialised.
     */
    static QString configDir();

    /**
     * The path a user typed, made absolute: a leading `~` is the home
     * directory, and the path is cleaned.  Empty for a path that is still
     * relative, as `open_file()` takes absolute paths only and LogSquirl's
     * working directory is nothing the user chose (`/` when started from
     * the Finder or the Dock).
     */
    static QString absoluteUserPath( const QString& path );

    // ── Instance methods ─────────────────────────────────────────────

    /**
     * Open the log file and the serial port and start reading.  No-op if
     * already running.
     *
     * A save path is appended to, so an earlier capture in that file is
     * never overwritten.  One that is not a full path (absoluteUserPath())
     * is refused before anything is written.
     *
     * On failure the reason has been emitted through errorOccurred(), and
     * a log file that start() created is removed again.
     *
     * @return true if the port is open, false if the session did not start.
     */
    bool start();

    /**
     * Close the serial port, dropping output the device has not taken yet,
     * so that closing cannot wait for it.  No-op if not running.
     */
    void stop();

    /**
     * Send data to the serial port and log it as a TX line.
     *
     * Appends the configured line ending (txLineEnding) to the data before
     * transmission.  The sent command is also written to the log file with
     * a [TX] marker so it appears in the LogSquirl tab, after the part of a
     * line received so far, which is written as a line of its own.
     *
     * @param data  Raw bytes to send (line ending is appended automatically).
     * @return true if the data was written successfully, false otherwise.
     */
    bool sendData( const QByteArray& data );

    /**
     * Prevent the temporary log file from being deleted when this
     * object is destroyed.  Call before deleteLater() so that the
     * LogSquirl tab can keep displaying the captured output.
     *
     * @return The temporary directory left on disk, or empty when the
     *         session writes to a save path (nothing to preserve).
     */
    QString preserveTempFile();

    /**
     * Rotate the log file: close the current temp file and open a new
     * one in the same temp directory.  The old file is preserved so the
     * existing LogSquirl tab keeps its content.  New serial output is
     * redirected to the new file.
     *
     * If the new file cannot be created, errorOccurred() is emitted and
     * the capture continues in the old file; if that cannot be reopened
     * either, the session is stopped (finished() is emitted).
     *
     * @param partialLine  Where the part of a line received so far goes.
     * @return Absolute path to the new temp file, or empty on failure.
     */
    QString rotateLog( PartialLine partialLine = PartialLine::WriteOut );

    /** Whether the serial port is currently open. */
    bool isRunning() const;

    /** The port name this session is attached to. */
    const QString& portName() const
    {
        return config_.portName;
    }

    /** Return the current configuration. */
    const SerialConfig& config() const
    {
        return config_;
    }

    /** Update the TX line ending for subsequent sendData() calls. */
    void setTxLineEnding( TxLineEnding ending )
    {
        config_.txLineEnding = ending;
    }

    /**
     * Absolute path to the log file (save path or temp file).
     *
     * Returns the user-configured save path when one was provided,
     * otherwise the temporary file path.  This is the file that will
     * be opened in LogSquirl via host->open_file().  It is only valid
     * after start() is called.
     */
    QString tempFilePath() const;

    /** Whether the session writes directly to a user-specified save path. */
    bool isUsingSavePath() const
    {
        return usingSavePath_;
    }

    // ── Counters ─────────────────────────────────────────────────────
    //
    // What the session has done since start(): read them at any time, from
    // the main thread.  None of them emits a signal when it changes, as most
    // change on every line; a view that shows them live reads them on a
    // timer of its own.

    /** Lines written to the current log file; 0 again after each rotation. */
    qint64 lineCount() const
    {
        return lineCount_;
    }

    /**
     * Lines written to all the session's log files: received ones and the
     * [TX] lines of sendData().  Not reset by a rotation.
     */
    qint64 totalLineCount() const
    {
        return totalLineCount_;
    }

    /** Bytes received from the device, as they came, before line splitting. */
    qint64 receivedBytes() const
    {
        return receivedBytes_;
    }

    /**
     * Size of the current log file, in bytes: what the session wrote to it
     * (timestamps and escapes included), plus what a save path appended to
     * held already.  0 again after each rotation.
     */
    qint64 fileSize() const
    {
        return fileBytes_;
    }

    /** How often the log has been rotated (rotateLog() that succeeded). */
    int rotationCount() const
    {
        return rotationCount_;
    }

    /**
     * File size at which sizeLimitReached() is emitted next: the limit
     * (SerialConfig::maxFileBytes) after start() or a rotation, further on
     * after a rotation that failed.  0 without a limit, or before start().
     */
    qint64 nextRotationSize() const
    {
        return config_.maxFileBytes > 0 ? nextRotationAt_ : 0;
    }

    /** When start() last opened the port; invalid before. */
    const QDateTime& startTime() const
    {
        return startTime_;
    }

Q_SIGNALS:
    /** Emitted when the serial port has been opened successfully. */
    void started();

    /** Emitted when the serial port is closed. */
    void finished();

    /** Emitted when an error occurs (port not found, permission denied, …). */
    void errorOccurred( const QString& message );

    /** Emitted after data has been sent to the serial port. */
    void dataSent( const QByteArray& data );

    /**
     * Emitted, from the event loop, once the log file has reached
     * SerialConfig::maxFileBytes: the receiver rotates it (rotateLog()).
     * When that fails, emitted again once the file has grown by the limit
     * once more, not on every line.
     */
    void sizeLimitReached();

private Q_SLOTS:
    /** Handle new data available on the serial port. */
    void onReadyRead();

    /**
     * Handle serial port errors.  Any error of the open port but a
     * TimeoutError (device unplugged, driver failure) is reported once and
     * stops the session.
     */
    void onPortError( QSerialPort::SerialPortError error );

private:
    /// Write one line to the log file, timestamped if configured and with
    /// its control characters escaped (escapeControls()); false,
    /// after failWrite(), if the log file cannot be written.
    bool writeLine( const QByteArray& line );

    /// Flush the log file; false, after failWrite(), if it cannot be written.
    bool flushLogFile();

    /// The log file cannot be written (disk full, volume gone): say so once
    /// and stop the session, rather than drop every line from here on.
    void failWrite();

    /** Write out a buffered partial line, e.g. before the file is closed. */
    void flushPartialLine();

    /// Open @p path for the capture with @p mode, refusing anything that is
    /// not a regular file: a FIFO would block the UI thread, a symlink lead
    /// elsewhere, a device take the capture.  A file it creates is readable
    /// by the user only.  False, with the reason in @p error.
    bool openLogFile( const QString& path, QIODevice::OpenMode mode, QString& error );

    /** Close the log file after a failed start; remove it if start() created it. */
    void discardLogFile();

    SerialConfig config_;
    QString savePath_;

    QSerialPort port_;
    QTemporaryDir tempDir_;
    QFile tempFile_;
    /// The log file open in tempFile_, which an open by descriptor leaves
    /// without a name; empty while none is.
    QString logPath_;
    QByteArray readBuffer_;       ///< Accumulates partial lines from the port.
    bool readAfterCr_ = false;    ///< Whether the data read so far ended with a CR.
    qint64 lineCount_ = 0;        ///< Lines in the current log file.
    qint64 totalLineCount_ = 0;   ///< Lines in all log files since start().
    qint64 receivedBytes_ = 0;    ///< Bytes read from the port since start().
    QDateTime startTime_;         ///< When start() opened the port.
    int rotationCount_ = 0;       ///< Incremented on each successful rotateLog().
    bool usingSavePath_ = false;  ///< True when writing directly to the log directory.
    bool createdLogFile_ = false; ///< True when start() created the log file.
    bool portFailed_ = false;     ///< True once the open port reported an error.
    bool writeFailed_ = false;    ///< True once a write to the log file failed.
    qint64 fileBytes_ = 0;        ///< Size of the log file being written.
    qint64 nextRotationAt_ = 0;   ///< Size at which sizeLimitReached() is due.
};

} // namespace serial_monitor
