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
 * @file capturelog_test.cpp
 * @brief BDD tests for CaptureLog: what a capture writes to its log file.
 *
 * Bytes are handed to a CaptureLog as a port would deliver them, in reads
 * that cut lines (and CRLF pairs, and UTF-8 characters) anywhere, and the
 * log is written to a QBuffer: no port, no pseudo-terminal, no event loop,
 * so they run on every platform.
 *
 * Devices end lines with "\n", "\r\n" or a lone "\r", and some never end
 * them at all (binary data, or a progress display that redraws with "\r"),
 * so the part of a line kept must not grow without bound.  A CR ends its
 * line at once, even when it is the last byte read: a device that ends
 * lines with a lone CR must not see each line appear one line late.
 */

#include <catch2/catch.hpp>

#include "capturelog.h"

#include <QBuffer>

using serial_monitor::CaptureLog;

namespace {

/// A CaptureLog writing to a buffer, without timestamps unless asked.
struct Capture {
    explicit Capture( qsizetype maxLineLength = CaptureLog::kMaxLineLength )
        : log( false, {}, maxLineLength )
    {
        file.open( QIODevice::WriteOnly );
        log.start( &file );
    }

    /// What has been written to the log file.
    const QByteArray& written() const
    {
        return file.data();
    }

    QBuffer file;
    CaptureLog log;
};

/// A file that is open for writing and refuses every write, as a full disk.
class RefusingDevice : public QIODevice {
public:
    RefusingDevice()
    {
        open( QIODevice::WriteOnly );
    }

protected:
    qint64 readData( char*, qint64 ) override
    {
        return -1;
    }

    qint64 writeData( const char*, qint64 ) override
    {
        return -1;
    }
};

} // namespace

SCENARIO( "received bytes are written line by line", "[capturelog]" )
{
    GIVEN( "a capture" )
    {
        Capture capture;

        WHEN( "it receives LF-terminated lines and a partial last line" )
        {
            REQUIRE( capture.log.receive( "first\nsecond\nthird" ) );

            THEN( "the complete lines are written, and the partial line is kept" )
            {
                REQUIRE( capture.written() == "first\nsecond\n" );
                REQUIRE( capture.log.hasPartialLine() );
            }

            AND_WHEN( "the partial line is ended" )
            {
                REQUIRE( capture.log.endPartialLine() );

                THEN( "it is written as a line of its own" )
                {
                    REQUIRE( capture.written() == "first\nsecond\nthird\n" );
                    REQUIRE_FALSE( capture.log.hasPartialLine() );
                }
            }
        }

        WHEN( "it receives CRLF-terminated lines" )
        {
            REQUIRE( capture.log.receive( "first\r\nsecond\r\n" ) );

            THEN( "each CRLF ends exactly one line" )
            {
                REQUIRE( capture.written() == "first\nsecond\n" );
                REQUIRE_FALSE( capture.log.hasPartialLine() );
            }
        }

        WHEN( "it receives lines ended by a lone CR" )
        {
            REQUIRE( capture.log.receive( "first\rsecond\rthird" ) );

            THEN( "a lone CR ends a line too" )
            {
                REQUIRE( capture.written() == "first\nsecond\n" );
                REQUIRE( capture.log.hasPartialLine() );
            }
        }

        WHEN( "the device ends its lines with a lone CR, then goes quiet" )
        {
            REQUIRE( capture.log.receive( "first\rsecond\r" ) );

            THEN( "the last line is written at once, not when the next one arrives" )
            {
                REQUIRE( capture.written() == "first\nsecond\n" );
                REQUIRE_FALSE( capture.log.hasPartialLine() );
            }
        }

        WHEN( "the LF of a CRLF pair arrives in a later read" )
        {
            REQUIRE( capture.log.receive( "first\r" ) );
            const auto before = capture.written();
            REQUIRE( capture.log.receive( "\nsecond\n" ) );

            THEN( "the CR ended the line at once, and the LF adds no empty line" )
            {
                REQUIRE( before == "first\n" );
                REQUIRE( capture.written() == "first\nsecond\n" );
            }
        }

        WHEN( "a read ending in CR is followed by one that does not start with LF" )
        {
            REQUIRE( capture.log.receive( "first\r" ) );
            REQUIRE( capture.log.receive( "second\r\n" ) );

            THEN( "the CR ended the first line, and the second line is complete" )
            {
                REQUIRE( capture.written() == "first\nsecond\n" );
            }
        }

        WHEN( "a read ending in CR is followed by a CRLF pair's LF and another LF" )
        {
            REQUIRE( capture.log.receive( "first\r" ) );
            REQUIRE( capture.log.receive( "\n\n" ) );

            THEN( "only the LF that completes the CRLF pair is skipped" )
            {
                REQUIRE( capture.written() == "first\n\n" );
            }
        }

        WHEN( "the LF of a CRLF pair arrives only after an empty read" )
        {
            REQUIRE( capture.log.receive( "first\r" ) );
            REQUIRE( capture.log.receive( "" ) );
            REQUIRE( capture.log.receive( "\nsecond\n" ) );

            THEN( "it still completes the CRLF pair" )
            {
                REQUIRE( capture.written() == "first\nsecond\n" );
            }
        }

        WHEN( "it receives empty lines in every line ending style" )
        {
            REQUIRE( capture.log.receive( "\n\r\n\r\r\n" ) );

            THEN( "each empty line is kept" )
            {
                REQUIRE( capture.written() == "\n\n\n\n" );
            }
        }

        WHEN( "it receives data without any line terminator" )
        {
            REQUIRE( capture.log.receive( "partial" ) );

            THEN( "nothing is written yet" )
            {
                REQUIRE( capture.written().isEmpty() );
                REQUIRE( capture.log.hasPartialLine() );
            }
        }

        WHEN( "nothing is partial and the partial line is ended" )
        {
            REQUIRE( capture.log.receive( "done\n" ) );
            REQUIRE( capture.log.endPartialLine() );

            THEN( "no empty line is added" )
            {
                REQUIRE( capture.written() == "done\n" );
            }
        }
    }
}

SCENARIO( "a line that never ends is forced out at the maximum length", "[capturelog]" )
{
    GIVEN( "a capture whose lines may be 8 bytes long" )
    {
        Capture capture( 8 );

        WHEN( "it receives more unterminated data than that" )
        {
            REQUIRE( capture.log.receive( "done\n0123456789" ) );

            THEN( "the unterminated data is written as a line of its own" )
            {
                REQUIRE( capture.written() == "done\n0123456789\n" );
                REQUIRE_FALSE( capture.log.hasPartialLine() );
            }
        }

        WHEN( "it receives unterminated data within the maximum" )
        {
            REQUIRE( capture.log.receive( "01234567" ) );

            THEN( "the data keeps waiting for its terminator" )
            {
                REQUIRE( capture.written().isEmpty() );
                REQUIRE( capture.log.hasPartialLine() );
            }
        }
    }

    GIVEN( "a capture with the default maximum" )
    {
        Capture capture;

        WHEN( "a binary stream without line endings arrives" )
        {
            REQUIRE( capture.log.receive( QByteArray( CaptureLog::kMaxLineLength + 1, 'x' ) ) );

            THEN( "it is written instead of growing the partial line" )
            {
                REQUIRE( capture.log.lineCount() == 1 );
                REQUIRE( capture.written().size() == CaptureLog::kMaxLineLength + 2 );
                REQUIRE_FALSE( capture.log.hasPartialLine() );
            }
        }
    }
}

SCENARIO( "a line forced out does not split a UTF-8 character", "[capturelog]" )
{
    GIVEN( "a capture whose lines may be 8 bytes long" )
    {
        Capture capture( 8 );

        WHEN( "the data over the maximum ends in the first byte of a character" )
        {
            // "€" is E2 82 AC; the read ended after its first byte.
            REQUIRE( capture.log.receive( "0123456789\xE2" ) );

            THEN( "the line ends before the character, which waits for its other bytes" )
            {
                REQUIRE( capture.written() == "0123456789\n" );
                REQUIRE( capture.log.hasPartialLine() );
            }

            AND_WHEN( "the rest of the character arrives with the end of the line" )
            {
                REQUIRE( capture.log.receive( "\x82\xAC end\n" ) );

                THEN( "the character is whole in the next line" )
                {
                    REQUIRE( capture.written() == "0123456789\n\xE2\x82\xAC end\n" );
                }
            }
        }

        WHEN( "the data over the maximum ends in the middle of a four-byte character" )
        {
            // U+1F600 is F0 9F 98 80.
            REQUIRE( capture.log.receive( "0123456789\xF0\x9F\x98" ) );
            REQUIRE( capture.log.receive( "\x80\n" ) );

            THEN( "the character's bytes wait for its last one" )
            {
                REQUIRE( capture.written() == "0123456789\n\xF0\x9F\x98\x80\n" );
            }
        }

        WHEN( "the data over the maximum ends with a complete character" )
        {
            REQUIRE( capture.log.receive( "0123456789\xE2\x82\xAC" ) );

            THEN( "all of it is written" )
            {
                REQUIRE( capture.written() == "0123456789\xE2\x82\xAC\n" );
                REQUIRE_FALSE( capture.log.hasPartialLine() );
            }
        }

        WHEN( "garbage over the maximum is not UTF-8 at all" )
        {
            REQUIRE( capture.log.receive( "\x80\x80\x80\x80\x80\x80\x80\x80\x80" ) );

            THEN( "all of it is written, so the partial line cannot grow" )
            {
                REQUIRE( capture.log.lineCount() == 1 );
                REQUIRE_FALSE( capture.log.hasPartialLine() );
            }
        }
    }
}

SCENARIO( "control characters are written as visible escapes", "[capturelog]" )
{
    GIVEN( "a capture" )
    {
        Capture capture;

        WHEN( "a device boots with NUL padding and baud-mismatch garbage" )
        {
            REQUIRE( capture.log.receive( QByteArray( "\0\0\0\x02\xF8\x1B\x7F\x80ready\n", 14 ) ) );

            THEN( "the control bytes are \\xNN escapes, the other bytes as they came" )
            {
                REQUIRE( capture.written() == "\\x00\\x00\\x00\\x02\xF8\\x1B\x7F\x80ready\n" );
            }
        }

        WHEN( "it receives colour codes, tabs and other C0 controls" )
        {
            REQUIRE( capture.log.receive( "\x1B[32mOK\x1B[0m\na\tb\x01\x1F\n" ) );

            THEN( "every control but tab is escaped, with upper-case hex digits" )
            {
                REQUIRE( capture.written() == "\\x1B[32mOK\\x1B[0m\na\tb\\x01\\x1F\n" );
            }
        }

        WHEN( "it receives text that looks like an escape, and other characters" )
        {
            REQUIRE( capture.log.receive( "plain text \\x00\n\xE2\x82\xAC \xFF\xFE \x7F\n" ) );

            THEN( "it is left alone" )
            {
                REQUIRE( capture.written() == "plain text \\x00\n\xE2\x82\xAC \xFF\xFE \x7F\n" );
            }
        }

        WHEN( "a command with a control character is sent" )
        {
            REQUIRE( capture.log.send( "ctrl-c \x03" ) );

            THEN( "it is escaped in its [TX] line too" )
            {
                REQUIRE( capture.written() == "[TX] ctrl-c \\x03\n" );
            }
        }
    }
}

SCENARIO( "each line is timestamped with the time it is written", "[capturelog]" )
{
    GIVEN( "a capture with timestamps and a clock the test sets" )
    {
        auto now = QDateTime( QDate( 2026, 3, 25 ), QTime( 14, 30, 0, 123 ) );
        QBuffer file;
        file.open( QIODevice::WriteOnly );
        CaptureLog log( true, [ &now ]() { return now; } );
        log.start( &file );

        WHEN( "a line is received" )
        {
            REQUIRE( log.receive( "boot\n" ) );

            THEN( "it is written after the time in brackets" )
            {
                REQUIRE( file.data() == "[2026-03-25 14:30:00.123] boot\n" );
            }

            THEN( "the file's size counts the timestamp" )
            {
                REQUIRE( log.fileSize() == file.data().size() );
            }
        }

        WHEN( "a line arrives in two reads, a second apart" )
        {
            REQUIRE( log.receive( "par" ) );
            now = now.addSecs( 1 );
            REQUIRE( log.receive( "tial\n" ) );

            THEN( "its time is when it was completed" )
            {
                REQUIRE( file.data() == "[2026-03-25 14:30:01.123] partial\n" );
            }
        }

        WHEN( "a command is sent" )
        {
            REQUIRE( log.send( "reboot" ) );

            THEN( "its [TX] line is timestamped too" )
            {
                REQUIRE( file.data() == "[2026-03-25 14:30:00.123] [TX] reboot\n" );
            }
        }
    }
}

SCENARIO( "a sent command is written between received lines", "[capturelog]" )
{
    GIVEN( "a capture that has received part of a line" )
    {
        Capture capture;
        REQUIRE( capture.log.receive( "boot\nfoo" ) );

        WHEN( "a command is sent, and the device then ends its line" )
        {
            REQUIRE( capture.log.send( "cmd" ) );
            REQUIRE( capture.log.receive( "bar\n" ) );

            THEN( "the part received before the command stays before it" )
            {
                REQUIRE( capture.written() == "boot\nfoo\n[TX] cmd\nbar\n" );
            }
        }
    }

    GIVEN( "a capture that has received part of a line, ending inside a character" )
    {
        Capture capture;
        REQUIRE( capture.log.receive( "boot\n25 \xC2" ) ); // the first byte of a degree sign

        WHEN( "a command is sent, and the device then ends its line" )
        {
            REQUIRE( capture.log.send( "cmd" ) );
            REQUIRE( capture.log.receive( "\xB0"
                                          "C\n" ) );

            THEN( "the character is kept whole, after the command" )
            {
                REQUIRE( capture.written()
                         == "boot\n25 \n[TX] cmd\n\xC2\xB0"
                            "C\n" );
            }
        }
    }

    GIVEN( "a capture whose received data ends with the first byte of a character only" )
    {
        Capture capture;
        REQUIRE( capture.log.receive( "boot\n\xC2" ) );

        WHEN( "a command is sent" )
        {
            REQUIRE( capture.log.send( "cmd" ) );

            THEN( "no empty line is written before it" )
            {
                REQUIRE( capture.written() == "boot\n[TX] cmd\n" );
                REQUIRE( capture.log.hasPartialLine() );
            }
        }
    }

    GIVEN( "a capture that received a line ended by a CR" )
    {
        Capture capture;
        REQUIRE( capture.log.receive( "first\r" ) );

        WHEN( "a command is sent before the LF of the CRLF pair arrives" )
        {
            REQUIRE( capture.log.send( "cmd" ) );
            REQUIRE( capture.log.receive( "\nsecond\n" ) );

            THEN( "the LF still adds no empty line" )
            {
                REQUIRE( capture.written() == "first\n[TX] cmd\nsecond\n" );
            }
        }
    }
}

SCENARIO( "a capture counts lines and bytes", "[capturelog][counters]" )
{
    GIVEN( "a capture that has not started" )
    {
        const CaptureLog log;

        THEN( "it has counted nothing" )
        {
            REQUIRE( log.lineCount() == 0 );
            REQUIRE( log.totalLineCount() == 0 );
            REQUIRE( log.receivedBytes() == 0 );
            REQUIRE( log.fileSize() == 0 );
            REQUIRE_FALSE( log.hasFailed() );
        }
    }

    GIVEN( "a capture" )
    {
        Capture capture;

        WHEN( "it receives two lines and part of a third" )
        {
            REQUIRE( capture.log.receive( "first\r\nsecond\nthi" ) );

            THEN( "the bytes received, the lines written and the file's size are counted" )
            {
                REQUIRE( capture.log.receivedBytes() == 17 );
                REQUIRE( capture.log.lineCount() == 2 );
                REQUIRE( capture.log.totalLineCount() == 2 );
                REQUIRE( capture.log.fileSize() == 13 ); // "first\nsecond\n"
            }

            AND_WHEN( "a command is sent" )
            {
                REQUIRE( capture.log.send( "cmd" ) );

                THEN( "the partial line and the [TX] line count, the command's bytes do not" )
                {
                    REQUIRE( capture.log.lineCount() == 4 );
                    REQUIRE( capture.log.totalLineCount() == 4 );
                    REQUIRE( capture.log.receivedBytes() == 17 );
                    REQUIRE( capture.log.fileSize() == 13 + 4 + 9 ); // "thi\n[TX] cmd\n"
                }
            }

            AND_WHEN( "it goes on in a new file, and the device ends its line" )
            {
                QBuffer next;
                next.open( QIODevice::WriteOnly );
                capture.log.startFile( &next );
                REQUIRE( capture.log.receive( "rd\n" ) );

                THEN( "the line goes whole to the new file" )
                {
                    REQUIRE( next.data() == "third\n" );
                    REQUIRE( capture.written() == "first\nsecond\n" );
                }

                THEN( "the new file's lines and size start again" )
                {
                    REQUIRE( capture.log.lineCount() == 1 );
                    REQUIRE( capture.log.fileSize() == 6 );
                }

                THEN( "the lines and bytes of the whole capture go on" )
                {
                    REQUIRE( capture.log.totalLineCount() == 3 );
                    REQUIRE( capture.log.receivedBytes() == 20 );
                }
            }

            AND_WHEN( "it is started again" )
            {
                QBuffer next;
                next.open( QIODevice::WriteOnly );
                capture.log.start( &next );
                REQUIRE( capture.log.receive( "new\n" ) );

                THEN( "every counter starts again, and the old partial line is dropped" )
                {
                    REQUIRE( next.data() == "new\n" );
                    REQUIRE( capture.log.lineCount() == 1 );
                    REQUIRE( capture.log.totalLineCount() == 1 );
                    REQUIRE( capture.log.receivedBytes() == 4 );
                    REQUIRE( capture.log.fileSize() == 4 );
                }
            }
        }
    }

    GIVEN( "a capture into a file that holds an earlier one" )
    {
        QBuffer file;
        file.setData( "earlier\n" );
        file.open( QIODevice::WriteOnly | QIODevice::Append );
        CaptureLog log( false );
        log.start( &file );

        WHEN( "a line is received" )
        {
            REQUIRE( log.receive( "new\n" ) );

            THEN( "the file's size counts what it held, the bytes received do not" )
            {
                REQUIRE( file.data() == "earlier\nnew\n" );
                REQUIRE( log.fileSize() == 12 );
                REQUIRE( log.receivedBytes() == 4 );
                REQUIRE( log.lineCount() == 1 );
            }
        }
    }
}

SCENARIO( "a log file that cannot be written fails the capture", "[capturelog]" )
{
    GIVEN( "a capture into a file that refuses writes" )
    {
        RefusingDevice file;
        CaptureLog log( false );
        log.start( &file );

        WHEN( "a line is received" )
        {
            const auto written = log.receive( "first\nsecond\n" );

            THEN( "it says so, and nothing is counted as written" )
            {
                REQUIRE_FALSE( written );
                REQUIRE( log.hasFailed() );
                REQUIRE( log.lineCount() == 0 );
                REQUIRE( log.fileSize() == 0 );
                REQUIRE( log.receivedBytes() == 13 );
            }

            AND_WHEN( "more is received, or a command sent" )
            {
                THEN( "every call fails" )
                {
                    REQUIRE_FALSE( log.receive( "third\n" ) );
                    REQUIRE_FALSE( log.send( "cmd" ) );
                }
            }

            AND_WHEN( "it is started again in a file that can be written" )
            {
                QBuffer next;
                next.open( QIODevice::WriteOnly );
                log.start( &next );

                THEN( "it writes again" )
                {
                    REQUIRE( log.receive( "again\n" ) );
                    REQUIRE_FALSE( log.hasFailed() );
                    REQUIRE( next.data() == "again\n" );
                }
            }
        }
    }

    GIVEN( "a capture that has not started" )
    {
        CaptureLog log( false );

        THEN( "it writes nothing, and says so" )
        {
            REQUIRE_FALSE( log.receive( "first\n" ) );
            REQUIRE_FALSE( log.send( "cmd" ) );
        }
    }
}
