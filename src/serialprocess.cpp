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
 *   2. start() has the session's CaptureLog open the log file, then opens
 *      the configured serial port with the given parameters (baud rate,
 *      data bits, stop bits, parity, flow control).
 *
 *   3. Incoming data is read via the readyRead signal and handed to the
 *      CaptureLog, which writes each complete line to the log file
 *      (capturelog.h).
 *
 *   4. The host opens the log file with follow/tail mode, so lines
 *      appear in real-time as the serial device sends data.
 *
 *   5. stop() closes the serial port, and the CaptureLog the file.
 */

#include "serialprocess.h"

#include <QDateTime>
#include <QSignalBlocker>

namespace serial_monitor {

namespace {

/// What a read asks for at least: a device that cannot be read says so then.
constexpr qint64 kMinReadSize = 4096;

} // namespace

QByteArray lineEndingBytes( TxLineEnding ending )
{
    switch ( ending ) {
    case TxLineEnding::CR:
        return QByteArrayLiteral( "\r" );
    case TxLineEnding::LF:
        return QByteArrayLiteral( "\n" );
    case TxLineEnding::CRLF:
        return QByteArrayLiteral( "\r\n" );
    case TxLineEnding::None:
        break;
    }
    return {};
}

// ── Construction / destruction ──────────────────────────────────────────

SerialProcess::SerialProcess( Host& host, const SerialConfig& config, const QString& savePath,
                              QObject* parent )
    : SerialProcess( host, config, savePath, std::make_unique<QSerialPort>(), parent )
{
}

SerialProcess::SerialProcess( Host& host, const SerialConfig& config, const QString& savePath,
                              std::unique_ptr<QIODevice> device, QObject* parent )
    : QObject( parent )
    , host_( host )
    , config_( config )
    , savePath_( savePath )
    , device_( device.release() )
    , port_( qobject_cast<QSerialPort*>( device_ ) )
    , log_( config.timestamps )
{
    device_->setParent( this );
    // What the event loop runs goes through guarded(): an exception must not
    // reach the host's event loop.
    connect( device_, &QIODevice::readyRead, this, [ this ]() {
        guarded( host_, "reading a serial port", [ this ] { onReadyRead(); } );
    } );
    if ( port_ ) {
        connect( port_, &QSerialPort::errorOccurred, this,
                 [ this ]( QSerialPort::SerialPortError error ) {
                     guarded( host_, "handling a serial port error",
                              [ this, error ] { onPortError( error ); } );
                 } );
    }

    // From the event loop, not from within the slot that is reading the
    // port; and not once the file has been rotated meanwhile.
    log_.setSizeLimit( config_.maxFileBytes, [ this ]() {
        QMetaObject::invokeMethod(
            this,
            [ this ]() {
                guarded( host_, "reaching a serial capture's size limit", [ this ] {
                    if ( log_.hasReachedSizeLimit() && isRunning() ) {
                        Q_EMIT sizeLimitReached();
                    }
                } );
            },
            Qt::QueuedConnection );
    } );
}

SerialProcess::~SerialProcess()
{
    // The owner may be half-destroyed already (QObject deletes its children
    // after the owner's own destructor has run), so stopping must not emit
    // finished() into it.
    blockSignals( true );
    stop();

    // The device is a child, destroyed after the members its handlers use;
    // whatever it might still signal must not reach this object.
    device_->disconnect( this );
}

// ── Static: port discovery ──────────────────────────────────────────────

QStringList SerialProcess::discoverPorts()
{
    return filterPorts( QSerialPortInfo::availablePorts() );
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

// ── Instance: start / stop ──────────────────────────────────────────────

bool SerialProcess::start()
{
    if ( isRunning() ) {
        return true;
    }

    // One log file, never two: the save path, or a temporary file.
    QString error;
    if ( !log_.open( config_.portName, savePath_, host_.tempRoot(), error ) ) {
        Q_EMIT errorOccurred( error );
        return false;
    }
    portFailed_ = false;
    writeFailed_ = false;

    // Configure the serial port
    if ( port_ ) {
        port_->setPortName( config_.portName );
        port_->setBaudRate( config_.baudRate );
        port_->setDataBits( config_.dataBits );
        port_->setStopBits( config_.stopBits );
        port_->setParity( config_.parity );
        port_->setFlowControl( config_.flowControl );
    }

    if ( !device_->open( QIODevice::ReadWrite ) ) {
        Q_EMIT errorOccurred( QString( "Failed to open port %1: %2" )
                                  .arg( config_.portName, device_->errorString() ) );
        log_.discard();
        return false;
    }
    startTime_ = QDateTime::currentDateTime();

    host_.log( LOGSQUIRL_LOG_INFO,
               QString( "Opened %1 at %2 baud" ).arg( config_.portName ).arg( config_.baudRate ) );
    Q_EMIT started();
    return true;
}

void SerialProcess::stop()
{
    if ( !isRunning() ) {
        return;
    }

    // Output the device has not taken yet (flow control holding it back, a
    // pulled cable) is dropped: the last close of a tty waits for it to
    // drain, on Linux up to 30 seconds, and this runs on the UI thread.
    // An error of a port being closed (a pulled adapter fails the clear)
    // is not reported: the session ends anyway, and its caller says why.
    {
        const QSignalBlocker blocker( device_ );
        if ( port_ ) {
            port_->clear( QSerialPort::Output );
        }
        device_->close();
    }

    // Write out any remaining partial line, and close the file
    logged( log_.close() );

    host_.log( LOGSQUIRL_LOG_INFO, QString( "Closed %1 (%2 lines captured)" )
                                       .arg( config_.portName )
                                       .arg( totalLineCount() ) );
    Q_EMIT finished();
}

bool SerialProcess::sendData( const QByteArray& data, TxLineEnding lineEnding )
{
    if ( !isRunning() ) {
        return false;
    }

    const auto written = device_->write( data + lineEndingBytes( lineEnding ) );
    if ( written < 0 ) {
        // A port error from within write() has said why and ended the session.
        if ( !portFailed_ ) {
            Q_EMIT errorOccurred( QString( "Failed to write to %1: %2" )
                                      .arg( config_.portName, device_->errorString() ) );
        }
        return false;
    }

    logged( log_.send( data ) );

    Q_EMIT dataSent( data );
    return true;
}

QString SerialProcess::preserveTempFile()
{
    return log_.keepTempDir();
}

QString SerialProcess::rotateLog( PartialLine partialLine )
{
    if ( !isRunning() ) {
        return {};
    }

    QString error;
    const auto newPath = log_.rotate( partialLine, error );
    // The partial line written out before the rotation may have failed.
    logged( !log_.hasFailed() );
    if ( newPath.isEmpty() ) {
        Q_EMIT errorOccurred( error );
        // A capture left running without a file would drop everything.
        if ( !log_.isOpen() ) {
            stop();
        }
        return {};
    }

    host_.log( LOGSQUIRL_LOG_INFO, QString( "Rotated serial log for %1 (rotation #%2)" )
                                       .arg( config_.portName )
                                       .arg( rotationCount() ) );

    return newPath;
}

bool SerialProcess::isRunning() const
{
    return device_->isOpen();
}

// ── Private helpers ─────────────────────────────────────────────────────

void SerialProcess::logged( bool ok )
{
    if ( !ok ) {
        failWrite();
    }
}

void SerialProcess::failWrite()
{
    if ( writeFailed_ ) {
        return;
    }
    writeFailed_ = true;
    // A port that failed has said why the session ends, and stops it.
    if ( portFailed_ ) {
        return;
    }
    Q_EMIT errorOccurred( log_.writeError() + "; capture stopped." );
    // From the event loop, not from within the slot that is reading the port.
    queueStop();
}

void SerialProcess::failPort( const QString& message )
{
    // A session that is ending already has said why.
    if ( portFailed_ || writeFailed_ ) {
        return;
    }
    // Stop from the event loop, not from within the device's own error
    // handling.  The receiver logs and shows the message; logging it here
    // as well would report the error twice.
    portFailed_ = true;
    Q_EMIT errorOccurred( message );
    queueStop();
}

void SerialProcess::queueStop()
{
    QMetaObject::invokeMethod(
        this,
        [ this ]() { guarded( host_, "stopping a failed serial capture", [ this ] { stop(); } ); },
        Qt::QueuedConnection );
}

void SerialProcess::onReadyRead()
{
    // All the device holds.  QSerialPort reports its errors through
    // errorOccurred(); another device (one a test plays) by a read that fails.
    QByteArray data( qMax( device_->bytesAvailable(), kMinReadSize ), Qt::Uninitialized );
    const auto read = device_->read( data.data(), data.size() );
    if ( read < 0 ) {
        failPort( QString( "Serial port error on %1: %2; capture stopped." )
                      .arg( config_.portName, device_->errorString() ) );
        return;
    }
    data.truncate( read );
    logged( log_.receive( data ) );
}

void SerialProcess::onPortError( QSerialPort::SerialPortError error )
{
    // NoError is emitted on successful operations, and a TimeoutError only
    // ends a waitFor...() call, which the session never makes: ignore both.
    // While the port is not open, the error comes from open(), and start()
    // reports it.
    if ( error == QSerialPort::NoError || error == QSerialPort::TimeoutError || !port_->isOpen() ) {
        return;
    }

    // Any other error ends the session: the device is gone (unplugged,
    // powered off) or the driver failed, and the port will not deliver data
    // again - some drivers report an unplug as a ReadError or an
    // UnknownError, and the read notifier then fires on every pass of the
    // event loop.
    if ( error == QSerialPort::ResourceError ) {
        failPort( QString( "%1 was disconnected; capture stopped." ).arg( config_.portName ) );
    }
    else {
        failPort( QString( "Serial port error on %1: %2; capture stopped." )
                      .arg( config_.portName, port_->errorString() ) );
    }
}

} // namespace serial_monitor
