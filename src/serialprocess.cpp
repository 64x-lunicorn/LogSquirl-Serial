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
 * @file serialprocess.cpp
 * @brief Implementation of SerialProcess — serial port discovery and data streaming.
 *
 * HOW IT WORKS
 * ────────────
 *   1. discoverPorts() enumerates serial ports via QSerialPortInfo and
 *      filters out Bluetooth and virtual entries.
 *
 *   2. start() opens the configured serial port with the given parameters
 *      (baud rate, data bits, stop bits, parity, flow control).
 *
 *   3. Incoming data is read via the readyRead signal.  Each complete line
 *      is optionally timestamped and written to a temporary file.
 *
 *   4. The host opens the temporary file with follow/tail mode, so lines
 *      appear in real-time as the serial device sends data.
 *
 *   5. stop() closes the serial port.
 */

#include "serialprocess.h"
#include "plugin.h"

#include <QDateTime>
#include <QSettings>
#include <QStandardPaths>

namespace serial_monitor {

// ── Construction / destruction ──────────────────────────────────────────

SerialProcess::SerialProcess( const SerialConfig& config,
                              const QString& savePath,
                              QObject* parent )
    : QObject( parent )
    , config_( config )
    , savePath_( savePath )
{
    connect( &port_, &QSerialPort::readyRead,
             this, &SerialProcess::onReadyRead );
    connect( &port_, &QSerialPort::errorOccurred,
             this, &SerialProcess::onPortError );
}

SerialProcess::~SerialProcess()
{
    stop();
}

// ── Static: port discovery ──────────────────────────────────────────────

QString SerialProcess::configDir()
{
    if ( g_state.api && g_state.handle ) {
        return QString::fromUtf8( g_state.api->get_config_dir( g_state.handle ) );
    }
    return QStandardPaths::writableLocation( QStandardPaths::TempLocation );
}

QStringList SerialProcess::discoverPorts()
{
    const auto allPorts = QSerialPortInfo::availablePorts();
    const auto ports = filterPorts( allPorts );

    hostLog( LOGSQUIRL_LOG_INFO,
             qPrintable( QString( "Discovered %1 serial port(s)." ).arg( ports.size() ) ) );
    return ports;
}

QStringList SerialProcess::filterPorts( const QList<QSerialPortInfo>& ports )
{
    QStringList result;
    for ( const auto& info : ports ) {
        // Skip Bluetooth serial ports (virtual, not useful for log capture)
        const auto desc = info.description().toLower();
        const auto mfr = info.manufacturer().toLower();
        if ( desc.contains( "bluetooth" ) || mfr.contains( "bluetooth" ) ) {
            continue;
        }

        // Skip ports with no system location (phantom entries)
        if ( info.systemLocation().isEmpty() ) {
            continue;
        }

        result.append( info.portName() );
    }
    return result;
}

SerialConfig SerialProcess::defaultConfig()
{
    SerialConfig cfg;

    // Load persisted defaults from config if available
    const auto cfgDir = configDir();
    if ( !cfgDir.isEmpty() ) {
        QSettings settings( cfgDir + "/serial.ini", QSettings::IniFormat );
        cfg.baudRate = settings.value( "serial/defaultBaud", 115200 ).toInt();
        cfg.timestamps = settings.value( "serial/timestamps", true ).toBool();
    }

    return cfg;
}

// ── Instance: start / stop ──────────────────────────────────────────────

void SerialProcess::start()
{
    if ( isRunning() ) {
        return;
    }

    if ( !tempDir_.isValid() ) {
        Q_EMIT errorOccurred( "Failed to create temporary directory." );
        return;
    }

    // Open the temporary file for writing
    const auto tempPath = tempDir_.path() + "/serial_" + config_.portName + ".log";
    tempFile_.setFileName( tempPath );
    if ( !tempFile_.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        Q_EMIT errorOccurred( "Failed to open temp file: " + tempFile_.errorString() );
        return;
    }

    // Optionally open the user-specified save file
    if ( !savePath_.isEmpty() ) {
        saveFile_.setFileName( savePath_ );
        if ( !saveFile_.open( QIODevice::WriteOnly | QIODevice::Append ) ) {
            hostLog( LOGSQUIRL_LOG_WARNING,
                     qPrintable( "Cannot open save file: " + saveFile_.errorString() ) );
        }
    }

    lineCount_ = 0;
    readBuffer_.clear();

    // Configure the serial port
    port_.setPortName( config_.portName );
    port_.setBaudRate( config_.baudRate );
    port_.setDataBits( config_.dataBits );
    port_.setStopBits( config_.stopBits );
    port_.setParity( config_.parity );
    port_.setFlowControl( config_.flowControl );

    if ( !port_.open( QIODevice::ReadOnly ) ) {
        Q_EMIT errorOccurred(
            QString( "Failed to open port %1: %2" )
                .arg( config_.portName, port_.errorString() ) );
        tempFile_.close();
        return;
    }

    hostLog( LOGSQUIRL_LOG_INFO,
             qPrintable( QString( "Opened %1 at %2 baud" )
                             .arg( config_.portName )
                             .arg( config_.baudRate ) ) );
    Q_EMIT started();
}

void SerialProcess::stop()
{
    if ( !isRunning() ) {
        return;
    }

    port_.close();

    // Flush any remaining partial line
    if ( !readBuffer_.isEmpty() ) {
        if ( config_.timestamps ) {
            const auto ts = QDateTime::currentDateTime().toString( "yyyy-MM-dd HH:mm:ss.zzz" );
            tempFile_.write( "[" + ts.toUtf8() + "] " );
            if ( saveFile_.isOpen() ) {
                saveFile_.write( "[" + ts.toUtf8() + "] " );
            }
        }
        tempFile_.write( readBuffer_ );
        tempFile_.write( "\n", 1 );
        tempFile_.flush();

        if ( saveFile_.isOpen() ) {
            saveFile_.write( readBuffer_ );
            saveFile_.write( "\n", 1 );
            saveFile_.flush();
        }

        ++lineCount_;
        readBuffer_.clear();
    }

    tempFile_.close();
    if ( saveFile_.isOpen() ) {
        saveFile_.close();
    }

    hostLog( LOGSQUIRL_LOG_INFO,
             qPrintable( QString( "Closed %1 (%2 lines captured)" )
                             .arg( config_.portName )
                             .arg( lineCount_ ) ) );
    Q_EMIT finished();
}

void SerialProcess::preserveTempFile()
{
    tempDir_.setAutoRemove( false );
}

bool SerialProcess::isRunning() const
{
    return port_.isOpen();
}

QString SerialProcess::tempFilePath() const
{
    return tempFile_.fileName();
}

// ── Private slots ───────────────────────────────────────────────────────

void SerialProcess::onReadyRead()
{
    readBuffer_.append( port_.readAll() );

    int start = 0;
    for ( int i = 0; i < readBuffer_.size(); ++i ) {
        if ( readBuffer_[ i ] == '\n' ) {
            const auto lineData = readBuffer_.mid( start, i - start );
            start = i + 1;

            // Optionally prepend timestamp
            if ( config_.timestamps ) {
                const auto ts
                    = QDateTime::currentDateTime().toString( "yyyy-MM-dd HH:mm:ss.zzz" );
                const auto prefix = "[" + ts.toUtf8() + "] ";
                tempFile_.write( prefix );
                if ( saveFile_.isOpen() ) {
                    saveFile_.write( prefix );
                }
            }

            // Write line data to temp file
            tempFile_.write( lineData );
            tempFile_.write( "\n", 1 );
            tempFile_.flush();

            // Write to save file if open
            if ( saveFile_.isOpen() ) {
                saveFile_.write( lineData );
                saveFile_.write( "\n", 1 );
                saveFile_.flush();
            }

            ++lineCount_;
        }
    }

    // Keep any incomplete trailing data in the buffer
    if ( start > 0 ) {
        readBuffer_.remove( 0, start );
    }
}

void SerialProcess::onPortError( QSerialPort::SerialPortError error )
{
    // NoError is emitted on successful operations — ignore it
    if ( error == QSerialPort::NoError ) {
        return;
    }

    const auto msg = QString( "Serial port error on %1: %2" )
                         .arg( config_.portName, port_.errorString() );
    hostLog( LOGSQUIRL_LOG_ERROR, qPrintable( msg ) );
    Q_EMIT errorOccurred( msg );
}

} // namespace serial_monitor
