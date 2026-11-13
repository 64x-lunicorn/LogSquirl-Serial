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
 * @file capturelog.cpp
 * @brief Implementation of CaptureLog — the log file, its rotation, line
 *        framing, [TX] lines and counters.
 */

#include "capturelog.h"
#include "tempdirs.h"

#include <QDir>
#include <QFileDevice>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>

#include <utility>

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

/**
 * Remove every complete line from the front of @p buffer and return the
 * lines, without their terminator.  "\n", "\r\n" and a lone "\r" all end a
 * line.  A CR ends its line at once, even as the last byte, and sets
 * @p afterCr: an LF at the start of the next call is then the second half
 * of a CRLF pair and is skipped.  An incomplete last line stays in the
 * buffer, unless it is longer than @p maxLineLength: then it is returned
 * as a line of its own, ending before a UTF-8 character whose last bytes
 * have not arrived yet.
 */
QList<QByteArray> takeLines( QByteArray& buffer, bool& afterCr, qsizetype maxLineLength )
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

/**
 * @p line with every C0 control character but tab written as a visible
 * `\xNN` escape, so that the log file holds text only: NUL padding or
 * garbage a device sends cannot make LogSquirl take the whole file for
 * UTF-16 or another encoding.  Other bytes, a backslash among them, are
 * left as they are.
 */
QByteArray escapeControls( const QByteArray& line )
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

} // namespace

CaptureLog::CaptureLog( bool timestamps, Clock clock, qsizetype maxLineLength )
    : timestamps_( timestamps )
    , clock_( clock ? std::move( clock ) : Clock( [] { return QDateTime::currentDateTime(); } ) )
    , maxLineLength_( maxLineLength )
{
}

CaptureLog::~CaptureLog() = default;

// ── File names ──────────────────────────────────────────────────────────

QString CaptureLog::generateLogPath( const QString& dir, const QString& portName,
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

QString CaptureLog::absoluteUserPath( const QString& path )
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

// ── The log file ────────────────────────────────────────────────────────

bool CaptureLog::open( const QString& portName, const QString& savePath, const QString& tempRoot,
                       QString& error )
{
    // The path the user typed is checked first: it is theirs to fix,
    // whatever else would fail.
    if ( !savePath.isEmpty() && absoluteUserPath( savePath ).isEmpty() ) {
        error = QString( "The log file %1 is not a full path; give one such as %2." )
                    .arg( savePath, QDir( QDir::homePath() ).filePath( "serial.log" ) );
        return false;
    }

    // Nothing is ever truncated: a save path is appended to, so that Stop
    // and Start with the same path keep the earlier capture, and the temp
    // file must be new.
    QString path;
    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    if ( !savePath.isEmpty() ) {
        path = absoluteUserPath( savePath );
        QDir().mkpath( QFileInfo( path ).absolutePath() );
        mode |= QIODevice::Append;
    }
    else {
        if ( !tempDir_ ) {
            tempDir_ = std::make_unique<QTemporaryDir>( tempDirTemplate( tempRoot ) );
        }
        if ( !tempDir_->isValid() ) {
            error = QStringLiteral( "Failed to create temporary directory." );
            return false;
        }
        path = tempDir_->filePath( "serial_" + safeFileName( portName ) + ".log" );
        mode |= QIODevice::NewOnly;
    }

    createdFile_ = !QFileInfo::exists( path );
    QString openError;
    if ( !openFile( path, mode, openError ) ) {
        error = "Failed to open log file " + path + ": " + openError;
        file_.setFileName( {} );
        path_.clear();
        return false;
    }
    portName_ = portName;
    usingSavePath_ = !savePath.isEmpty();
    saveDir_ = usingSavePath_ ? QFileInfo( path ).absolutePath() : QString();
    rotationCount_ = 0;
    start( &file_ );
    return true;
}

void CaptureLog::discard()
{
    file_.close();
    if ( createdFile_ && !path_.isEmpty() ) {
        QFile::remove( path_ );
    }
    file_.setFileName( {} );
    path_.clear();
}

bool CaptureLog::close()
{
    const auto written = endPartialLine();
    file_.close();
    return written;
}

QString CaptureLog::rotate( PartialLine partialLine, QString& error )
{
    if ( device_ != &file_ || !file_.isOpen() ) {
        return {};
    }

    if ( partialLine == PartialLine::WriteOut ) {
        // A failure is hasFailed()'s, for the caller to tell.
        endPartialLine();
    }

    // In the log directory, a new timestamped file; in the temp dir, the
    // next number.  Either way the file must be new: opening an existing
    // one would truncate an earlier capture.
    const auto newPath = usingSavePath_ ? generateLogPath( saveDir_, portName_ )
                                        : tempDir_->filePath( QString( "serial_%1_%2.log" )
                                                                  .arg( safeFileName( portName_ ) )
                                                                  .arg( rotationCount_ + 1 ) );

    // Close the old file (it stays on disk for the old tab)
    const auto oldPath = path_;
    file_.close();

    QString reason;
    if ( !openFile( newPath, QIODevice::WriteOnly | QIODevice::NewOnly, reason ) ) {
        // Keep capturing into the old file: a capture left running with a
        // closed file would silently drop everything from here on.
        QString reopenError;
        if ( openFile( oldPath, QIODevice::WriteOnly | QIODevice::Append, reopenError ) ) {
            error = QString( "Could not rotate the log to %1 (%2); still writing to %3." )
                        .arg( newPath, reason, oldPath );
        }
        else {
            error = QString( "Could not rotate the log to %1 (%2), nor reopen %3 (%4); "
                             "capture stopped." )
                        .arg( newPath, reason, oldPath, reopenError );
        }
        return {};
    }

    ++rotationCount_;
    startFile( &file_ );
    return newPath;
}

QString CaptureLog::keepTempDir()
{
    if ( usingSavePath_ || !tempDir_ ) {
        return {};
    }
    tempDir_->setAutoRemove( false );
    return tempDir_->path();
}

bool CaptureLog::openFile( const QString& path, QIODevice::OpenMode mode, QString& error )
{
    file_.setFileName( path );
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
    if ( !file_.open( fd, mode & ~QIODevice::NewOnly, QFileDevice::AutoCloseHandle ) ) {
        ::close( fd );
        error = file_.errorString();
        return false;
    }
    path_ = path;
    return true;
#else
    // Checked before the open, which a rename between the two could race;
    // Windows has no open that refuses a link or a device by itself.
    const QFileInfo info( path );
    if ( info.isSymLink() || ( info.exists() && !info.isFile() ) ) {
        error = QStringLiteral( "not a regular file" );
        return false;
    }
    if ( !file_.open( mode ) ) {
        error = file_.errorString();
        return false;
    }
    if ( mode.testFlag( QIODevice::NewOnly ) || !info.exists() ) {
        file_.setPermissions( QFileDevice::ReadOwner | QFileDevice::WriteOwner );
    }
    path_ = path;
    return true;
#endif
}

// ── The size limit ──────────────────────────────────────────────────────

void CaptureLog::setSizeLimit( qint64 bytes, SizeLimitReached reached )
{
    sizeLimit_ = bytes;
    sizeLimitReached_ = std::move( reached );
}

// ── Writing ─────────────────────────────────────────────────────────────

void CaptureLog::start( QIODevice* file )
{
    partial_.clear();
    afterCr_ = false;
    failed_ = false;
    writeError_.clear();
    totalLineCount_ = 0;
    receivedBytes_ = 0;
    startFile( file );
}

void CaptureLog::startFile( QIODevice* file )
{
    device_ = file;
    lineCount_ = 0;
    // A file appended to counts with what it holds already.
    fileSize_ = file ? file->size() : 0;
    nextRotationAt_ = sizeLimit_;
}

bool CaptureLog::receive( const QByteArray& data )
{
    receivedBytes_ += data.size();
    partial_.append( data );

    const auto lines = takeLines( partial_, afterCr_, maxLineLength_ );
    for ( const auto& line : lines ) {
        if ( !writeLine( line ) ) {
            return false;
        }
    }
    return flush();
}

bool CaptureLog::send( const QByteArray& command )
{
    // The part of a line received so far is written first, as a line of
    // its own: the rest of it, arriving after the command, would otherwise
    // be joined to it below the [TX] line, under a later timestamp.  A
    // character the device has only begun stays for the rest of the line.
    const auto complete = completeUtf8Length( partial_ );
    if ( complete > 0 && !writeLine( partial_.left( complete ) ) ) {
        return false;
    }
    partial_.remove( 0, complete );
    return writeLine( "[TX] " + command ) && flush();
}

bool CaptureLog::endPartialLine()
{
    if ( partial_.isEmpty() ) {
        return true;
    }
    const auto line = std::exchange( partial_, {} );
    return writeLine( line ) && flush();
}

bool CaptureLog::writeLine( const QByteArray& line )
{
    if ( failed_ || !device_ ) {
        return false;
    }
    QByteArray record;
    if ( timestamps_ ) {
        record = "[" + clock_().toString( "yyyy-MM-dd HH:mm:ss.zzz" ).toUtf8() + "] ";
    }
    record += escapeControls( line );
    record += '\n';
    if ( device_->write( record ) != record.size() ) {
        fail();
        return false;
    }
    ++lineCount_;
    ++totalLineCount_;
    fileSize_ += record.size();
    // A rotation that fails keeps the capture in this file: it is asked for
    // again once the file has grown by the limit once more, as asking on
    // every line would report the failure on every line.
    if ( sizeLimit_ > 0 && fileSize_ >= nextRotationAt_ ) {
        nextRotationAt_ = fileSize_ + sizeLimit_;
        if ( sizeLimitReached_ ) {
            sizeLimitReached_();
        }
    }
    return true;
}

bool CaptureLog::flush()
{
    if ( failed_ || !device_ ) {
        return false;
    }
    auto* fileDevice = qobject_cast<QFileDevice*>( device_ );
    if ( fileDevice && !fileDevice->flush() ) {
        fail();
        return false;
    }
    return true;
}

void CaptureLog::fail()
{
    failed_ = true;
    const auto path = device_ == &file_ ? path_ : QString();
    writeError_
        = QString( "Could not write the log file %1 (%2)" ).arg( path, device_->errorString() );
}

} // namespace serial_monitor
