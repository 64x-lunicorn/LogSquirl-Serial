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
 * @brief Implementation of CaptureLog — line framing, [TX] lines and counters.
 */

#include "capturelog.h"

#include <QFileDevice>
#include <QList>

#include <utility>

namespace serial_monitor {

namespace {

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

void CaptureLog::start( QIODevice* file )
{
    partial_.clear();
    afterCr_ = false;
    failed_ = false;
    totalLineCount_ = 0;
    receivedBytes_ = 0;
    startFile( file );
}

void CaptureLog::startFile( QIODevice* file )
{
    file_ = file;
    lineCount_ = 0;
    // A file appended to counts with what it holds already.
    fileSize_ = file ? file->size() : 0;
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
    if ( failed_ || !file_ ) {
        return false;
    }
    QByteArray record;
    if ( timestamps_ ) {
        record = "[" + clock_().toString( "yyyy-MM-dd HH:mm:ss.zzz" ).toUtf8() + "] ";
    }
    record += escapeControls( line );
    record += '\n';
    if ( file_->write( record ) != record.size() ) {
        failed_ = true;
        return false;
    }
    ++lineCount_;
    ++totalLineCount_;
    fileSize_ += record.size();
    return true;
}

bool CaptureLog::flush()
{
    if ( failed_ || !file_ ) {
        return false;
    }
    auto* fileDevice = qobject_cast<QFileDevice*>( file_ );
    if ( fileDevice && !fileDevice->flush() ) {
        failed_ = true;
        return false;
    }
    return true;
}

} // namespace serial_monitor
