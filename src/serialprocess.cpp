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
#include "tempdirs.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

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

/**
 * The length of the longest start of @p data that does not end inside a
 * UTF-8 character: all of it, unless its last bytes begin a character
 * whose remaining bytes are still to come.  Bytes that are not UTF-8 count
 * as complete.
 */
qsizetype completeUtf8Length( const QByteArray& data )
{
    const auto size = data.size();
    // A character is at most four bytes: look at its first byte, at most
    // three back from the end.
    for ( qsizetype back = 1; back <= 3 && back <= size; ++back ) {
        const auto byte = static_cast<unsigned char>( data.at( size - back ) );
        if ( ( byte & 0xC0 ) == 0x80 ) {
            continue; // a continuation byte
        }
        qsizetype length = 1;
        if ( ( byte & 0xE0 ) == 0xC0 ) {
            length = 2;
        }
        else if ( ( byte & 0xF0 ) == 0xE0 ) {
            length = 3;
        }
        else if ( ( byte & 0xF8 ) == 0xF0 ) {
            length = 4;
        }
        return length > back ? size - back : size;
    }
    return size;
}

} // namespace

// ── Construction / destruction ──────────────────────────────────────────

SerialProcess::SerialProcess( const SerialConfig& config, const QString& savePath, QObject* parent )
    : QObject( parent )
    , config_( config )
    , savePath_( savePath )
    , tempDir_( tempDirTemplate( tempRoot() ) )
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
        // A character cut in two would be garbled at the end of this line
        // and the start of the next: its first bytes wait for the rest.
        auto length = completeUtf8Length( buffer );
        if ( length == 0 ) {
            length = buffer.size();
        }
        lines.append( buffer.left( length ) );
        buffer.remove( 0, length );
    }
    return lines;
}

QByteArray SerialProcess::escapeControls( const QByteArray& line )
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    QByteArray escaped;
    escaped.reserve( line.size() );
    for ( const char c : line ) {
        const auto byte = static_cast<unsigned char>( c );
        if ( byte < 0x20 && c != '\t' ) {
            escaped.append( "\\x" );
            escaped.append( kHex[ byte >> 4 ] );
            escaped.append( kHex[ byte & 0x0F ] );
        }
        else {
            escaped.append( c );
        }
    }
    return escaped;
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

    lineCount_ = 0;
    readBuffer_.clear();
    readAfterCr_ = false;
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
    if ( writeLine( "[TX] " + data ) ) {
        flushLogFile();
    }

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
    return logPath_;
}

// ── Private helpers ─────────────────────────────────────────────────────

bool SerialProcess::writeLine( const QByteArray& line )
{
    if ( writeFailed_ ) {
        return false;
    }
    QByteArray record;
    if ( config_.timestamps ) {
        const auto ts = QDateTime::currentDateTime().toString( "yyyy-MM-dd HH:mm:ss.zzz" );
        record = "[" + ts.toUtf8() + "] ";
    }
    record += escapeControls( line );
    record += '\n';
    if ( tempFile_.write( record ) != record.size() ) {
        failWrite();
        return false;
    }
    ++lineCount_;
    return true;
}

bool SerialProcess::flushLogFile()
{
    if ( writeFailed_ ) {
        return false;
    }
    if ( !tempFile_.flush() ) {
        failWrite();
        return false;
    }
    return true;
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

void SerialProcess::flushPartialLine()
{
    if ( readBuffer_.isEmpty() ) {
        return;
    }

    if ( writeLine( readBuffer_ ) ) {
        flushLogFile();
    }
    readBuffer_.clear();
}

// ── Private slots ───────────────────────────────────────────────────────

void SerialProcess::onReadyRead()
{
    readBuffer_.append( port_.readAll() );

    const auto lines = takeLines( readBuffer_, readAfterCr_ );
    for ( const auto& line : lines ) {
        if ( !writeLine( line ) ) {
            return;
        }
    }
    flushLogFile();
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
