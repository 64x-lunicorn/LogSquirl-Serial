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
 *   proc->start();                    // opens serial port
 *   qDebug() << proc->tempFilePath(); // LogSquirl opens this file
 *   proc->stop();                     // closes port
 */

#pragma once

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
 * Populated from the PortWidget UI and passed to the SerialProcess
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
};

/**
 * Manages a single serial port session.
 *
 * Captures incoming data line-by-line and writes each line to:
 *   1. A temporary file inside a QTemporaryDir (always).
 *   2. A user-selected log file (optional, if savePath is non-empty).
 *
 * The temporary file path is what the host opens via open_file().
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
    explicit SerialProcess( const SerialConfig& config,
                            const QString& savePath = {},
                            QObject* parent = nullptr );
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

    /**
     * Return a sensible default configuration (115200 8N1, no FC, timestamps on).
     */
    static SerialConfig defaultConfig();

    /**
     * Return the plugin's config directory from the host API.
     * Falls back to a temp path if the plugin is not initialised.
     */
    static QString configDir();

    // ── Instance methods ─────────────────────────────────────────────

    /** Open the serial port and start reading.  No-op if already running. */
    void start();

    /** Close the serial port.  No-op if not running. */
    void stop();

    /**
     * Prevent the temporary log file from being deleted when this
     * object is destroyed.  Call before deleteLater() so that the
     * LogSquirl tab can keep displaying the captured output.
     */
    void preserveTempFile();

    /**
     * Rotate the log file: close the current temp file and open a new
     * one in the same temp directory.  The old file is preserved so the
     * existing LogSquirl tab keeps its content.  New serial output is
     * redirected to the new file.
     *
     * @return Absolute path to the new temp file, or empty on failure.
     */
    QString rotateLog();

    /** Whether the serial port is currently open. */
    bool isRunning() const;

    /** The port name this session is attached to. */
    const QString& portName() const { return config_.portName; }

    /**
     * Absolute path to the temporary log file.
     *
     * This is the file that will be opened in LogSquirl via
     * host->open_file().  It is only valid after start() is called.
     */
    QString tempFilePath() const;

    /** Total number of lines captured so far. */
    qint64 lineCount() const { return lineCount_; }

  Q_SIGNALS:
    /** Emitted when the serial port has been opened successfully. */
    void started();

    /** Emitted when the serial port is closed. */
    void finished();

    /** Emitted when an error occurs (port not found, permission denied, …). */
    void errorOccurred( const QString& message );

  private Q_SLOTS:
    /** Handle new data available on the serial port. */
    void onReadyRead();

    /** Handle serial port errors. */
    void onPortError( QSerialPort::SerialPortError error );

  private:
    SerialConfig config_;
    QString savePath_;

    QSerialPort port_;
    QTemporaryDir tempDir_;
    QFile tempFile_;
    QFile saveFile_;
    QByteArray readBuffer_;  ///< Accumulates partial lines from the port.
    qint64 lineCount_ = 0;
    int rotationCount_ = 0;  ///< Incremented on each rotateLog() call.
};

} // namespace serial_monitor
