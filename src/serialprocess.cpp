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
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace serial_monitor {

namespace {

/// Make a port name usable as part of a file name on every platform.
QString safeFileName( const QString& portName )
{
    auto name = portName;
    name.replace( QRegularExpression( "[^a-zA-Z0-9._-]" ), "_" );
    return name;
}

} // namespace

// ── Construction / destruction ──────────────────────────────────────────

SerialProcess::SerialProcess( const SerialConfig& config, const QString& savePath, QObject* parent )
    : QObject( parent )
    , config_( config )
    , savePath_( savePath )
{
    connect( &port_, &QSerialPort::readyRead, this, &SerialProcess::onReadyRead );
    connect( &port_, &QSerialPort::errorOccurred, this, &SerialProcess::onPortError );
}

SerialProcess::~SerialProcess()
{
    // The owner may be half-destroyed already (QObject deletes its children
    // after the owner's own destructor has run), so stopping must not emit
    // finished() into it.
    blockSignals( true );
    stop();

    // port_ is declared before the members its slots use, so it is
    // destroyed after them; whatever it might still signal must not
    // reach this object's slots.
    port_.disconnect( this );
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

    hostLog( LOGSQUIRL_LOG_INFO, QString( "Discovered %1 serial port(s)." ).arg( ports.size() ) );
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

QList<QByteArray> SerialProcess::takeLines( QByteArray& buffer, bool& afterCr,
                                            qsizetype maxLineLength )
{
    QList<QByteArray> lines;
    qsizetype start = 0;
    // The CR that ended the previous data may be the first half of a CRLF
    // whose LF arrives only now.  Its line has been taken already.
    if ( afterCr && buffer.startsWith( '\n' ) ) {
        start = 1;
    }
    if ( !buffer.isEmpty() ) {
        afterCr = false;
    }
    for ( qsizetype i = start; i < buffer.size(); ++i ) {
        const auto c = buffer.at( i );
        if ( c != '\n' && c != '\r' ) {
            continue;
        }
        lines.append( buffer.mid( start, i - start ) );
        if ( c == '\r' ) {
            if ( i + 1 == buffer.size() ) {
                afterCr = true;
            }
            else if ( buffer.at( i + 1 ) == '\n' ) {
                ++i;
            }
        }
        start = i + 1;
    }
    buffer.remove( 0, start );

    if ( buffer.size() > maxLineLength ) {
        lines.append( buffer );
        buffer.clear();
    }
    return lines;
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

QString SerialProcess::generateLogPath( const QString& dir, const QString& portName,
                                        const QDateTime& timestamp )
{
    const QDir logDir( dir );
    const auto stem = timestamp.toString( "yyyy-MM-dd_HHmmss" ) + "_" + safeFileName( portName );

    auto path = logDir.filePath( stem + ".log" );
    for ( int n = 2; QFileInfo::exists( path ); ++n ) {
        path = logDir.filePath( QString( "%1_%2.log" ).arg( stem ).arg( n ) );
    }
    return path;
}

// ── Instance: start / stop ──────────────────────────────────────────────

bool SerialProcess::start()
{
    if ( isRunning() ) {
        return true;
    }

    // When a save path is configured, write directly to the log directory
    // instead of creating a temporary file.  This avoids accumulating
    // orphaned temp files and ensures the user's log directory is used.
    //
    // Nothing is ever truncated: a save path is appended to, so that Stop
    // and Start with the same path keep the earlier capture, and the temp
    // file must be new.
    QString path;
    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    if ( !savePath_.isEmpty() ) {
        QDir().mkpath( QFileInfo( savePath_ ).absolutePath() );
        path = savePath_;
        mode |= QIODevice::Append;
    }
    else {
        if ( !tempDir_.isValid() ) {
            Q_EMIT errorOccurred( "Failed to create temporary directory." );
            return false;
        }
        path = tempDir_.filePath( "serial_" + safeFileName( config_.portName ) + ".log" );
        mode |= QIODevice::NewOnly;
    }

    createdLogFile_ = !QFileInfo::exists( path );
    tempFile_.setFileName( path );
    if ( !tempFile_.open( mode ) ) {
        Q_EMIT errorOccurred( "Failed to open log file: " + tempFile_.errorString() );
        tempFile_.setFileName( {} );
        return false;
    }
    usingSavePath_ = !savePath_.isEmpty();

    lineCount_ = 0;
    readBuffer_.clear();
    readAfterCr_ = false;
    deviceLost_ = false;

    // Configure the serial port
    port_.setPortName( config_.portName );
    port_.setBaudRate( config_.baudRate );
    port_.setDataBits( config_.dataBits );
    port_.setStopBits( config_.stopBits );
    port_.setParity( config_.parity );
    port_.setFlowControl( config_.flowControl );

    if ( !port_.open( QIODevice::ReadWrite ) ) {
        Q_EMIT errorOccurred(
            QString( "Failed to open port %1: %2" ).arg( config_.portName, port_.errorString() ) );
        discardLogFile();
        return false;
    }

    hostLog( LOGSQUIRL_LOG_INFO,
             QString( "Opened %1 at %2 baud" ).arg( config_.portName ).arg( config_.baudRate ) );
    Q_EMIT started();
    return true;
}

void SerialProcess::stop()
{
    if ( !isRunning() ) {
        return;
    }

    port_.close();

    // Flush any remaining partial line
    flushPartialLine();

    tempFile_.close();

    hostLog( LOGSQUIRL_LOG_INFO,
             QString( "Closed %1 (%2 lines captured)" ).arg( config_.portName ).arg( lineCount_ ) );
    Q_EMIT finished();
}

bool SerialProcess::sendData( const QByteArray& data )
{
    if ( !isRunning() ) {
        return false;
    }

    // Append the configured line ending
    QByteArray payload = data;
    switch ( config_.txLineEnding ) {
    case TxLineEnding::CR:
        payload.append( '\r' );
        break;
    case TxLineEnding::LF:
        payload.append( '\n' );
        break;
    case TxLineEnding::CRLF:
        payload.append( "\r\n" );
        break;
    case TxLineEnding::None:
        break;
    }

    const auto written = port_.write( payload );
    if ( written < 0 ) {
        Q_EMIT errorOccurred(
            QString( "Failed to write to %1: %2" ).arg( config_.portName, port_.errorString() ) );
        return false;
    }

    // Log the sent data as a [TX] line in the output file
    writeLine( "[TX] " + data );
    tempFile_.flush();

    Q_EMIT dataSent( data );
    return true;
}

void SerialProcess::preserveTempFile()
{
    // When writing directly to the log directory, the temp dir is unused
    // and can be auto-removed safely.
    if ( !usingSavePath_ ) {
        tempDir_.setAutoRemove( false );
    }
}

void SerialProcess::removeTempFiles()
{
    tempDir_.remove();
}

QString SerialProcess::rotateLog()
{
    if ( !isRunning() ) {
        return {};
    }

    // Flush any pending partial line to the old file before rotating
    flushPartialLine();

    // Generate the rotated file path.  When using the log directory,
    // create a new timestamped file there; otherwise use the temp dir.
    // Either way the file must be new: opening an existing one would
    // truncate an earlier capture.
    QString newPath;
    if ( usingSavePath_ ) {
        newPath = generateLogPath( QFileInfo( savePath_ ).absolutePath(), config_.portName );
    }
    else {
        newPath = tempDir_.filePath( QString( "serial_%1_%2.log" )
                                         .arg( safeFileName( config_.portName ) )
                                         .arg( rotationCount_ + 1 ) );
    }

    // Close the old file (it stays on disk for the old tab)
    const auto oldPath = tempFile_.fileName();
    tempFile_.close();

    tempFile_.setFileName( newPath );
    if ( !tempFile_.open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
        const auto reason = tempFile_.errorString();

        // Keep capturing into the old file: a session left running with a
        // closed file would silently drop everything from here on.
        tempFile_.setFileName( oldPath );
        if ( tempFile_.open( QIODevice::WriteOnly | QIODevice::Append ) ) {
            Q_EMIT errorOccurred( QString( "Could not rotate the log to %1 (%2); "
                                           "still writing to %3." )
                                      .arg( newPath, reason, oldPath ) );
        }
        else {
            Q_EMIT errorOccurred( QString( "Could not rotate the log to %1 (%2), nor reopen "
                                           "%3 (%4); capture stopped." )
                                      .arg( newPath, reason, oldPath, tempFile_.errorString() ) );
            stop();
        }
        return {};
    }

    ++rotationCount_;
    lineCount_ = 0;

    hostLog( LOGSQUIRL_LOG_INFO, QString( "Rotated serial log for %1 (rotation #%2)" )
                                     .arg( config_.portName )
                                     .arg( rotationCount_ ) );

    return newPath;
}

bool SerialProcess::isRunning() const
{
    return port_.isOpen();
}

QString SerialProcess::tempFilePath() const
{
    return tempFile_.fileName();
}

// ── Private helpers ─────────────────────────────────────────────────────

void SerialProcess::writeLine( const QByteArray& line )
{
    if ( config_.timestamps ) {
        const auto ts = QDateTime::currentDateTime().toString( "yyyy-MM-dd HH:mm:ss.zzz" );
        tempFile_.write( "[" + ts.toUtf8() + "] " );
    }
    tempFile_.write( line );
    tempFile_.write( "\n", 1 );
    ++lineCount_;
}

void SerialProcess::discardLogFile()
{
    tempFile_.close();
    if ( createdLogFile_ ) {
        tempFile_.remove();
    }
    tempFile_.setFileName( {} );
}

void SerialProcess::flushPartialLine()
{
    if ( readBuffer_.isEmpty() ) {
        return;
    }

    writeLine( readBuffer_ );
    tempFile_.flush();
    readBuffer_.clear();
}

// ── Private slots ───────────────────────────────────────────────────────

void SerialProcess::onReadyRead()
{
    readBuffer_.append( port_.readAll() );

    const auto lines = takeLines( readBuffer_, readAfterCr_ );
    for ( const auto& line : lines ) {
        writeLine( line );
    }
    tempFile_.flush();
}

void SerialProcess::onPortError( QSerialPort::SerialPortError error )
{
    // NoError is emitted on successful operations — ignore it.  While the
    // port is not open, the error comes from open(), and start() reports it.
    if ( error == QSerialPort::NoError || !port_.isOpen() || deviceLost_ ) {
        return;
    }

    if ( error == QSerialPort::ResourceError ) {
        // The device is gone (unplugged, powered off).  The port stays open
        // but will never deliver data again, so end the session - from the
        // event loop, not from within QSerialPort's own error handling.
        deviceLost_ = true;
        Q_EMIT errorOccurred(
            QString( "%1 was disconnected; capture stopped." ).arg( config_.portName ) );
        QMetaObject::invokeMethod( this, &SerialProcess::stop, Qt::QueuedConnection );
        return;
    }

    // The receiver logs and shows the message; logging it here as well
    // would report every error twice.
    Q_EMIT errorOccurred(
        QString( "Serial port error on %1: %2" ).arg( config_.portName, port_.errorString() ) );
}

} // namespace serial_monitor
