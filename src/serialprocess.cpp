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
 *   2. start() opens the log file and the configured serial port with the
 *      given parameters (baud rate, data bits, stop bits, parity, flow
 *      control).
 *
 *   3. Incoming data is read via the readyRead signal and handed to the
 *      session's CaptureLog, which writes each complete line to the log
 *      file (capturelog.h).
 *
 *   4. The host opens the log file with follow/tail mode, so lines
 *      appear in real-time as the serial device sends data.
 *
 *   5. stop() closes the serial port.
 */

#include "serialprocess.h"
#include "tempdirs.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSignalBlocker>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    : QObject( parent )
    , host_( host )
    , config_( config )
    , savePath_( savePath )
    , tempDir_( tempDirTemplate( host.tempRoot() ) )
    , log_( config.timestamps )
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

QString SerialProcess::absoluteUserPath( const QString& path )
{
    auto expanded = path.trimmed();
    if ( expanded == QStringLiteral( "~" ) ) {
        expanded = QDir::homePath();
    }
    else if ( expanded.startsWith( QStringLiteral( "~/" ) )
              || expanded.startsWith( QStringLiteral( "~\\" ) ) ) {
        expanded = QDir::homePath() + expanded.mid( 1 );
    }
    if ( expanded.isEmpty() || QDir::isRelativePath( expanded ) ) {
        return {};
    }
    return QDir::cleanPath( expanded );
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

    // The path the user typed is checked first: it is theirs to fix,
    // whatever else would fail.
    if ( !savePath_.isEmpty() && absoluteUserPath( savePath_ ).isEmpty() ) {
        Q_EMIT errorOccurred(
            QString( "The log file %1 is not a full path; give one such as %2." )
                .arg( savePath_, QDir( QDir::homePath() ).filePath( "serial.log" ) ) );
        return false;
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
        path = absoluteUserPath( savePath_ );
        QDir().mkpath( QFileInfo( path ).absolutePath() );
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
    QString openError;
    if ( !openLogFile( path, mode, openError ) ) {
        Q_EMIT errorOccurred( "Failed to open log file " + path + ": " + openError );
        tempFile_.setFileName( {} );
        logPath_.clear();
        return false;
    }
    usingSavePath_ = !savePath_.isEmpty();
    // A save path appended to counts with what it holds already.
    log_.start( &tempFile_ );
    nextRotationAt_ = config_.maxFileBytes;
    portFailed_ = false;
    writeFailed_ = false;

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
        const QSignalBlocker blocker( port_ );
        port_.clear( QSerialPort::Output );
        port_.close();
    }

    // Write out any remaining partial line
    logged( log_.endPartialLine() );

    tempFile_.close();

    host_.log(
        LOGSQUIRL_LOG_INFO,
        QString( "Closed %1 (%2 lines captured)" ).arg( config_.portName ).arg( lineCount() ) );
    Q_EMIT finished();
}

bool SerialProcess::sendData( const QByteArray& data, TxLineEnding lineEnding )
{
    if ( !isRunning() ) {
        return false;
    }

    const auto written = port_.write( data + lineEndingBytes( lineEnding ) );
    if ( written < 0 ) {
        // A port error from within write() has said why and ended the session.
        if ( !portFailed_ ) {
            Q_EMIT errorOccurred( QString( "Failed to write to %1: %2" )
                                      .arg( config_.portName, port_.errorString() ) );
        }
        return false;
    }

    logged( log_.send( data ) );

    Q_EMIT dataSent( data );
    return true;
}

QString SerialProcess::preserveTempFile()
{
    // When writing directly to the log directory, the temp dir is unused
    // and can be auto-removed safely.
    if ( usingSavePath_ ) {
        return {};
    }
    tempDir_.setAutoRemove( false );
    return tempDir_.path();
}

QString SerialProcess::rotateLog( PartialLine partialLine )
{
    if ( !isRunning() ) {
        return {};
    }

    if ( partialLine == PartialLine::WriteOut ) {
        logged( log_.endPartialLine() );
    }

    // Generate the rotated file path.  When using the log directory,
    // create a new timestamped file there; otherwise use the temp dir.
    // Either way the file must be new: opening an existing one would
    // truncate an earlier capture.
    QString newPath;
    if ( usingSavePath_ ) {
        newPath = generateLogPath( QFileInfo( absoluteUserPath( savePath_ ) ).absolutePath(),
                                   config_.portName );
    }
    else {
        newPath = tempDir_.filePath( QString( "serial_%1_%2.log" )
                                         .arg( safeFileName( config_.portName ) )
                                         .arg( rotationCount_ + 1 ) );
    }

    // Close the old file (it stays on disk for the old tab)
    const auto oldPath = logPath_;
    tempFile_.close();

    QString reason;
    if ( !openLogFile( newPath, QIODevice::WriteOnly | QIODevice::NewOnly, reason ) ) {
        // Keep capturing into the old file: a session left running with a
        // closed file would silently drop everything from here on.
        QString reopenError;
        if ( openLogFile( oldPath, QIODevice::WriteOnly | QIODevice::Append, reopenError ) ) {
            Q_EMIT errorOccurred( QString( "Could not rotate the log to %1 (%2); "
                                           "still writing to %3." )
                                      .arg( newPath, reason, oldPath ) );
        }
        else {
            Q_EMIT errorOccurred( QString( "Could not rotate the log to %1 (%2), nor reopen "
                                           "%3 (%4); capture stopped." )
                                      .arg( newPath, reason, oldPath, reopenError ) );
            stop();
        }
        return {};
    }

    ++rotationCount_;
    log_.startFile( &tempFile_ );
    nextRotationAt_ = config_.maxFileBytes;

    host_.log( LOGSQUIRL_LOG_INFO, QString( "Rotated serial log for %1 (rotation #%2)" )
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
    return logPath_;
}

// ── Private helpers ─────────────────────────────────────────────────────

void SerialProcess::logged( bool ok )
{
    if ( !ok ) {
        failWrite();
        return;
    }
    // A rotation that fails keeps the capture in this file: it is asked for
    // again once the file has grown by the limit once more, as asking on
    // every line would report the failure on every line.
    if ( config_.maxFileBytes > 0 && fileSize() >= nextRotationAt_ ) {
        nextRotationAt_ = fileSize() + config_.maxFileBytes;
        // From the event loop, not from within the slot that is reading the
        // port; and not once the file has been rotated meanwhile.
        QMetaObject::invokeMethod(
            this,
            [ this ]() {
                if ( fileSize() >= config_.maxFileBytes && isRunning() ) {
                    Q_EMIT sizeLimitReached();
                }
            },
            Qt::QueuedConnection );
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
    Q_EMIT errorOccurred( QString( "Could not write the log file %1 (%2); capture stopped." )
                              .arg( logPath_, tempFile_.errorString() ) );
    // From the event loop, not from within the slot that is reading the port.
    QMetaObject::invokeMethod( this, &SerialProcess::stop, Qt::QueuedConnection );
}

bool SerialProcess::openLogFile( const QString& path, QIODevice::OpenMode mode, QString& error )
{
    tempFile_.setFileName( path );
#ifdef Q_OS_UNIX
    // One open that cannot block on a FIFO nor follow a symlink, then a
    // check of what was opened: a check before the open could be raced.
    int flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    flags |= mode.testFlag( QIODevice::NewOnly ) ? O_EXCL : 0;
    flags |= mode.testFlag( QIODevice::Append ) ? O_APPEND : 0;
    const int fd = ::open( QFile::encodeName( path ).constData(), flags, S_IRUSR | S_IWUSR );
    if ( fd < 0 ) {
        // A FIFO without a reader (ENXIO) and a symlink (ELOOP) are what
        // the flags refuse; anything else is the system's own reason.
        if ( errno == ENXIO ) {
            error = QStringLiteral( "not a regular file" );
        }
        else if ( errno == ELOOP ) {
            error = QStringLiteral( "a symbolic link, not a regular file" );
        }
        else {
            error = QString::fromLocal8Bit( std::strerror( errno ) );
        }
        return false;
    }
    struct stat info{};
    if ( ::fstat( fd, &info ) != 0 || !S_ISREG( info.st_mode ) ) {
        ::close( fd );
        error = QStringLiteral( "not a regular file" );
        return false;
    }
    // A regular file never blocks; the flag only kept the open from waiting.
    ::fcntl( fd, F_SETFL, ::fcntl( fd, F_GETFL ) & ~O_NONBLOCK );
    if ( !tempFile_.open( fd, mode & ~QIODevice::NewOnly, QFileDevice::AutoCloseHandle ) ) {
        ::close( fd );
        error = tempFile_.errorString();
        return false;
    }
    logPath_ = path;
    return true;
#else
    // Checked before the open, which a rename between the two could race;
    // Windows has no open that refuses a link or a device by itself.
    const QFileInfo info( path );
    if ( info.isSymLink() || ( info.exists() && !info.isFile() ) ) {
        error = QStringLiteral( "not a regular file" );
        return false;
    }
    if ( !tempFile_.open( mode ) ) {
        error = tempFile_.errorString();
        return false;
    }
    if ( mode.testFlag( QIODevice::NewOnly ) || !info.exists() ) {
        tempFile_.setPermissions( QFileDevice::ReadOwner | QFileDevice::WriteOwner );
    }
    logPath_ = path;
    return true;
#endif
}

void SerialProcess::discardLogFile()
{
    tempFile_.close();
    if ( createdLogFile_ && !logPath_.isEmpty() ) {
        QFile::remove( logPath_ );
    }
    tempFile_.setFileName( {} );
    logPath_.clear();
}

// ── Private slots ───────────────────────────────────────────────────────

void SerialProcess::onReadyRead()
{
    logged( log_.receive( port_.readAll() ) );
}

void SerialProcess::onPortError( QSerialPort::SerialPortError error )
{
    // NoError is emitted on successful operations, and a TimeoutError only
    // ends a waitFor...() call, which the session never makes: ignore both.
    // While the port is not open, the error comes from open(), and start()
    // reports it.  A session that is ending already has said why.
    if ( error == QSerialPort::NoError || error == QSerialPort::TimeoutError || !port_.isOpen()
         || portFailed_ || writeFailed_ ) {
        return;
    }

    // Any other error ends the session: the device is gone (unplugged,
    // powered off) or the driver failed, and the port will not deliver data
    // again - some drivers report an unplug as a ReadError or an
    // UnknownError, and the read notifier then fires on every pass of the
    // event loop.  Stop from the event loop, not from within QSerialPort's
    // own error handling.  The receiver logs and shows the message; logging
    // it here as well would report the error twice.
    portFailed_ = true;
    if ( error == QSerialPort::ResourceError ) {
        Q_EMIT errorOccurred(
            QString( "%1 was disconnected; capture stopped." ).arg( config_.portName ) );
    }
    else {
        Q_EMIT errorOccurred( QString( "Serial port error on %1: %2; capture stopped." )
                                  .arg( config_.portName, port_.errorString() ) );
    }
    QMetaObject::invokeMethod( this, &SerialProcess::stop, Qt::QueuedConnection );
}

} // namespace serial_monitor
