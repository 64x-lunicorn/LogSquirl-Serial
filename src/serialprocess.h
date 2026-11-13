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
 * The serial capture is an adapter between the port and its CaptureLog
 * (capturelog.h), which owns the log file, its rotation and its size limit.
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
 *   auto* proc = new SerialProcess( host, cfg, "/optional/save.log", parent );
 *   if ( proc->start() )              // opens serial port
 *       qDebug() << proc->tempFilePath(); // LogSquirl opens this file
 *   proc->stop();                     // closes port
 */

#pragma once

#include "capturelog.h"
#include "host.h"

#include <QDateTime>
#include <QIODevice>
#include <QObject>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QString>
#include <QStringList>

#include <memory>

namespace serial_monitor {

/// Line ending appended to transmitted data (SerialProcess::sendData()).
enum class TxLineEnding {
    None, ///< No line ending appended.
    CR,   ///< Carriage return (\r).
    LF,   ///< Line feed (\n).
    CRLF  ///< Carriage return + line feed (\r\n).
};

/// The bytes of @p ending: empty for None.
QByteArray lineEndingBytes( TxLineEnding ending );

/// Bytes in a MiB, the unit the size limit is set in.
constexpr qint64 kBytesPerMiB = 1024 * 1024;

/// Size in MiB at which a log file is rotated unless the user chose another.
constexpr int kDefaultMaxFileMiB = 1024;

/**
 * Configuration for a serial port session.
 *
 * Taken from a saved configuration (profiles.h) and passed to the SerialProcess
 * constructor.  Sensible defaults match the most common embedded
 * device configuration: 115200 8N1, no flow control, timestamps on.
 */
struct SerialConfig {
    QString portName;
    qint32 baudRate = 115200;
    QSerialPort::DataBits dataBits = QSerialPort::Data8;
    QSerialPort::StopBits stopBits = QSerialPort::OneStop;
    QSerialPort::Parity parity = QSerialPort::NoParity;
    QSerialPort::FlowControl flowControl = QSerialPort::NoFlowControl;
    bool timestamps = true; ///< Prepend [YYYY-MM-DD HH:mm:ss.zzz] to each line.
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
 * avoiding orphaned temp files that fill up disk space.  Which file, and
 * what is written to it, is its CaptureLog's (capturelog.h).
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
     * @param host      What it logs to, and where its temporary directory
     *                  goes (Host::tempRoot()); must outlive it.
     * @param config    Serial port configuration.
     * @param savePath  Optional path to a .log file for persistent saving.
     *                  Pass an empty string to disable saving.
     * @param parent    QObject parent for memory management.
     */
    SerialProcess( Host& host, const SerialConfig& config, const QString& savePath = {},
                   QObject* parent = nullptr );

    /**
     * Construct a session that captures from @p device, which it takes,
     * in place of a serial port of @p config: for tests, which play the
     * device.  start() opens it for reading and writing (it must be closed
     * until then), and a read from it that fails ends the session as an
     * error of the port does.  A QSerialPort handed in is configured and
     * its errors are handled as those of the session's own port.
     */
    SerialProcess( Host& host, const SerialConfig& config, const QString& savePath,
                   std::unique_ptr<QIODevice> device, QObject* parent = nullptr );

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
    using PartialLine = CaptureLog::PartialLine;

    /// CaptureLog::generateLogPath(): a new timestamped log file in @p dir.
    static QString generateLogPath( const QString& dir, const QString& portName,
                                    const QDateTime& timestamp = QDateTime::currentDateTime() )
    {
        return CaptureLog::generateLogPath( dir, portName, timestamp );
    }

    /// CaptureLog::absoluteUserPath(): the path a user typed, made absolute.
    static QString absoluteUserPath( const QString& path )
    {
        return CaptureLog::absoluteUserPath( path );
    }

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
     * Appends @p lineEnding to the data before transmission.  The sent
     * command is also written to the log file with a [TX] marker so it
     * appears in the LogSquirl tab (CaptureLog::send()).
     *
     * @param data        Raw bytes to send, without a line ending.
     * @param lineEnding  What is appended to @p data on the wire; not logged.
     * @return true if the data was written successfully, false otherwise.
     */
    bool sendData( const QByteArray& data, TxLineEnding lineEnding );

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
     * Rotate the log file (CaptureLog::rotate()): close the current file
     * and open a new one beside it.  The old file is preserved so the
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

    /**
     * Absolute path to the log file (save path or temp file).
     *
     * Returns the user-configured save path when one was provided,
     * otherwise the temporary file path.  This is the file that will
     * be opened in LogSquirl via host->open_file().  It is only valid
     * after start() is called.
     */
    QString tempFilePath() const
    {
        return log_.path();
    }

    /** Whether the session writes directly to a user-specified save path. */
    bool isUsingSavePath() const
    {
        return log_.isUsingSavePath();
    }

    // ── Counters ─────────────────────────────────────────────────────
    //
    // What the session has done since start(): read them at any time, from
    // the main thread.  None of them emits a signal when it changes, as most
    // change on every line; a view that shows them live reads them on a
    // timer of its own.  All but the start time are its CaptureLog's.

    /** Lines written to the current log file; 0 again after each rotation. */
    qint64 lineCount() const
    {
        return log_.lineCount();
    }

    /**
     * Lines written to all the session's log files: received ones and the
     * [TX] lines of sendData().  Not reset by a rotation.
     */
    qint64 totalLineCount() const
    {
        return log_.totalLineCount();
    }

    /** Bytes received from the device, as they came, before line splitting. */
    qint64 receivedBytes() const
    {
        return log_.receivedBytes();
    }

    /**
     * Size of the current log file, in bytes: what the session wrote to it
     * (timestamps and escapes included), plus what a save path appended to
     * held already.  0 again after each rotation.
     */
    qint64 fileSize() const
    {
        return log_.fileSize();
    }

    /** How often the log has been rotated (rotateLog() that succeeded). */
    int rotationCount() const
    {
        return log_.rotationCount();
    }

    /**
     * File size at which sizeLimitReached() is emitted next: the limit
     * (SerialConfig::maxFileBytes) after start() or a rotation, further on
     * after a rotation that failed.  0 without a limit, or before start().
     */
    qint64 nextRotationSize() const
    {
        return log_.nextRotationSize();
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

private:
    /** Handle new data available on the device. */
    void onReadyRead();

    /**
     * Handle serial port errors.  Any error of the open port but a
     * TimeoutError (device unplugged, driver failure) is reported once and
     * stops the session.
     */
    void onPortError( QSerialPort::SerialPortError error );

    /// The device failed (@p message says how): say so once and stop the
    /// session, from the event loop.
    void failPort( const QString& message );

    /// After log_ wrote (@p ok: it could): fail the session if it could not.
    void logged( bool ok );

    /// The log file cannot be written (disk full, volume gone): say so once
    /// and stop the session, rather than drop every line from here on.
    void failWrite();

    Host& host_;
    SerialConfig config_;
    QString savePath_;

    QIODevice* device_;        ///< What is captured from: port_, or a device handed in.
    QSerialPort* port_;        ///< device_ as a serial port; null if it is none.
    CaptureLog log_;           ///< The log file, and what is written to it.
    QDateTime startTime_;      ///< When start() opened the port.
    bool portFailed_ = false;  ///< True once the open port reported an error.
    bool writeFailed_ = false; ///< True once a failed write to the log file was reported.
};

} // namespace serial_monitor
