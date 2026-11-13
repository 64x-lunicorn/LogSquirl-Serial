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
 *
 * The scenarios of the log file itself (the save path or the temporary
 * directory, regular files only, rotation, the size limit) write to real
 * files in temporary directories; those of a rotation that fails make the
 * directory read-only (readonlydir.h).
 */

#include <catch2/catch.hpp>

#include "capturelog.h"
#include "readonlydir.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <sys/types.h>
#endif

using serial_monitor::CaptureLog;
using serial_test::ReadOnlyDir;
using PartialLine = serial_monitor::CaptureLog::PartialLine;

namespace {

/// The port the file captures are named after.
const QString kPort = "/dev/ttyUSB0";

QByteArray readFile( const QString& path )
{
    QFile file( path );
    return file.open( QIODevice::ReadOnly ) ? file.readAll() : QByteArray();
}

void writeFile( const QString& path, const QByteArray& data )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    REQUIRE( file.write( data ) == data.size() );
}

/**
 * A CaptureLog open on a log file of its own, without timestamps: the save
 * path, or a temporary file below a temporary root of the test's own.  It
 * records the file's size at each request of the size limit, if it has one.
 */
struct FileCapture {
    explicit FileCapture( const QString& savePath = {}, qint64 sizeLimit = 0 )
    {
        log.setSizeLimit( sizeLimit, [ this ]() { requests << log.fileSize(); } );
        opened = log.open( kPort, savePath, root.path(), error );
    }

    QTemporaryDir root; ///< The temporary root; declared first, removed last.
    CaptureLog log{ false };
    bool opened = false;
    QString error;
    QList<qint64> requests; ///< fileSize() at each request of the size limit.
};

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
            REQUIRE( log.rotationCount() == 0 );
            REQUIRE( log.nextRotationSize() == 0 );
            REQUIRE_FALSE( log.hasFailed() );
            REQUIRE_FALSE( log.isOpen() );
            REQUIRE( log.path().isEmpty() );
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
                REQUIRE( log.writeError().startsWith( "Could not write the log file" ) );
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
                    REQUIRE( log.writeError().isEmpty() );
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

SCENARIO( "a capture opens a log file of its own", "[capturelog][files]" )
{
    GIVEN( "no save path" )
    {
        FileCapture capture;

        THEN( "it writes to a new file named after the port, in a directory of its own below "
              "the temporary root" )
        {
            REQUIRE( capture.opened );
            REQUIRE( capture.log.isOpen() );
            REQUIRE_FALSE( capture.log.isUsingSavePath() );
            const QFileInfo file( capture.log.path() );
            REQUIRE( file.fileName() == "serial__dev_ttyUSB0.log" );
            REQUIRE( file.dir().dirName().startsWith(
                QString( "logsquirl-serial-%1-" ).arg( QCoreApplication::applicationPid() ) ) );
            REQUIRE( QFileInfo( file.absolutePath() ).absolutePath()
                     == QFileInfo( capture.root.path() ).absoluteFilePath() );
        }

        WHEN( "a line is received" )
        {
            REQUIRE( capture.log.receive( "first\n" ) );

            THEN( "it is in the file at once" )
            {
                REQUIRE( readFile( capture.log.path() ) == "first\n" );
            }
        }

        WHEN( "the capture is closed with part of a line received" )
        {
            REQUIRE( capture.log.receive( "first\nsec" ) );
            REQUIRE( capture.log.close() );

            THEN( "the part is written as a line of its own, and the file kept" )
            {
                REQUIRE_FALSE( capture.log.isOpen() );
                REQUIRE( readFile( capture.log.path() ) == "first\nsec\n" );
            }
        }

#ifdef Q_OS_UNIX
        THEN( "the file is readable by the user only" )
        {
            struct stat info{};
            REQUIRE( ::stat( QFile::encodeName( capture.log.path() ).constData(), &info ) == 0 );
            REQUIRE( ( info.st_mode & 0777 ) == 0600 );
        }
#endif
    }

    GIVEN( "a capture into a temporary file, closed" )
    {
        QTemporaryDir root;
        QString error;
        QString path;
        QString kept;

        WHEN( "it is destroyed" )
        {
            {
                CaptureLog log( false );
                REQUIRE( log.open( kPort, {}, root.path(), error ) );
                path = log.path();
                log.close();
            }

            THEN( "its temporary directory is removed" )
            {
                REQUIRE_FALSE( QFileInfo::exists( path ) );
                REQUIRE_FALSE( QFileInfo::exists( QFileInfo( path ).absolutePath() ) );
            }
        }

        WHEN( "its temporary directory is kept, and it is destroyed" )
        {
            {
                CaptureLog log( false );
                REQUIRE( log.open( kPort, {}, root.path(), error ) );
                REQUIRE( log.receive( "first\n" ) );
                path = log.path();
                kept = log.keepTempDir();
                log.close();
            }

            THEN( "the directory and its file stay, for the tab that shows it" )
            {
                REQUIRE( kept == QFileInfo( path ).absolutePath() );
                REQUIRE( readFile( path ) == "first\n" );
                QDir( kept ).removeRecursively();
            }
        }
    }

    GIVEN( "a save path in a directory that does not exist yet" )
    {
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "logs/capture.log" );
        FileCapture capture( savePath );

        THEN( "the directory is created and the file written to" )
        {
            REQUIRE( capture.opened );
            REQUIRE( capture.log.isUsingSavePath() );
            REQUIRE( capture.log.path() == savePath );
            REQUIRE( capture.log.receive( "first\n" ) );
            REQUIRE( readFile( savePath ) == "first\n" );
        }

        THEN( "there is no temporary directory to keep" )
        {
            REQUIRE( capture.log.keepTempDir().isEmpty() );
        }
    }

    GIVEN( "a save path that already holds a capture" )
    {
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        writeFile( savePath, "earlier\n" );
        FileCapture capture( savePath );

        WHEN( "a line is received" )
        {
            REQUIRE( capture.log.receive( "new\n" ) );

            THEN( "it is appended, and the file's size counts what it held" )
            {
                REQUIRE( readFile( savePath ) == "earlier\nnew\n" );
                REQUIRE( capture.log.fileSize() == 12 );
            }
        }

        WHEN( "the capture is discarded, as when its port did not open" )
        {
            capture.log.discard();

            THEN( "the earlier capture is kept" )
            {
                REQUIRE( readFile( savePath ) == "earlier\n" );
                REQUIRE( capture.log.path().isEmpty() );
            }
        }
    }

    GIVEN( "a save path that does not exist yet, opened" )
    {
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        FileCapture capture( savePath );
        REQUIRE( capture.opened );

        WHEN( "the capture is discarded" )
        {
            capture.log.discard();

            THEN( "the file it created is removed" )
            {
                REQUIRE_FALSE( QFileInfo::exists( savePath ) );
                REQUIRE_FALSE( capture.log.isOpen() );
            }
        }
    }

    GIVEN( "a save path that is not a full path" )
    {
        FileCapture capture( "relative-capture.log" );

        THEN( "it is refused, saying why, and nothing is created in the working directory" )
        {
            REQUIRE_FALSE( capture.opened );
            REQUIRE( capture.error.contains( "not a full path" ) );
            REQUIRE_FALSE( QFileInfo::exists( "relative-capture.log" ) );
            REQUIRE( capture.log.path().isEmpty() );
        }
    }

    GIVEN( "a temporary root that refuses new directories" )
    {
        if ( !ReadOnlyDir::isEnforced() ) {
            WARN( "File permissions are not enforced (running as root?); skipped." );
            return;
        }
        QTemporaryDir root;
        CaptureLog log( false );
        QString error;
        bool opened = true;
        {
            const ReadOnlyDir readOnly( root.path() );
            opened = log.open( kPort, {}, root.path(), error );
        }

        THEN( "no file is opened, and it says why" )
        {
            REQUIRE_FALSE( opened );
            REQUIRE( error == "Failed to create temporary directory." );
        }
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "only a regular file is opened as the log file", "[capturelog][files]" )
{
    GIVEN( "a save path that is a FIFO" )
    {
        QTemporaryDir logDir;
        const auto fifo = logDir.filePath( "capture.log" );
        REQUIRE( ::mkfifo( QFile::encodeName( fifo ).constData(), 0600 ) == 0 );

        WHEN( "it is opened" )
        {
            QElapsedTimer timer;
            timer.start();
            FileCapture capture( fifo );

            THEN( "it is refused at once, without waiting for a reader" )
            {
                REQUIRE_FALSE( capture.opened );
                REQUIRE( timer.elapsed() < 2000 );
                REQUIRE( capture.error.contains( "not a regular file" ) );
                REQUIRE( capture.error.contains( fifo ) );
            }
        }
    }

    GIVEN( "a save path that is a character device" )
    {
        FileCapture capture( "/dev/null" );

        THEN( "it is refused" )
        {
            REQUIRE_FALSE( capture.opened );
            REQUIRE( capture.error.contains( "not a regular file" ) );
        }
    }

    GIVEN( "a save path that is a symlink to a file that does not exist" )
    {
        QTemporaryDir logDir;
        const auto target = logDir.filePath( "elsewhere.log" );
        const auto link = logDir.filePath( "capture.log" );
        REQUIRE( QFile::link( target, link ) );
        FileCapture capture( link );

        THEN( "it is refused, and the link's target is not created" )
        {
            REQUIRE_FALSE( capture.opened );
            REQUIRE_FALSE( QFileInfo::exists( target ) );
        }
    }

    GIVEN( "a save path that is a symlink to an existing log file" )
    {
        QTemporaryDir logDir;
        const auto target = logDir.filePath( "elsewhere.log" );
        writeFile( target, "earlier capture\n" );
        const auto link = logDir.filePath( "capture.log" );
        REQUIRE( QFile::link( target, link ) );
        FileCapture capture( link );

        THEN( "it is refused, and the target and the link are left as they were" )
        {
            REQUIRE_FALSE( capture.opened );
            REQUIRE( readFile( target ) == "earlier capture\n" );
            REQUIRE( QFileInfo( link ).isSymLink() );
        }
    }
}
#endif

SCENARIO( "a capture is rotated into a new file", "[capturelog][rotation]" )
{
    GIVEN( "a capture into a temporary file that has received two lines and part of a third" )
    {
        FileCapture capture;
        REQUIRE( capture.opened );
        const auto oldPath = capture.log.path();
        REQUIRE( capture.log.receive( "first\r\nsecond\nthi" ) );
        QString error;

        WHEN( "it is rotated, the partial line written out" )
        {
            const auto newPath = capture.log.rotate( PartialLine::WriteOut, error );

            THEN( "the capture goes on in the next file of its temporary directory" )
            {
                REQUIRE( error.isEmpty() );
                REQUIRE( newPath
                         == QFileInfo( oldPath ).dir().filePath( "serial__dev_ttyUSB0_1.log" ) );
                REQUIRE( capture.log.path() == newPath );
                REQUIRE( capture.log.isOpen() );
                REQUIRE( capture.log.rotationCount() == 1 );
            }

            THEN( "the old file keeps its content, the partial line ended in it" )
            {
                REQUIRE( readFile( oldPath ) == "first\nsecond\nthi\n" );
                REQUIRE( readFile( newPath ).isEmpty() );
            }

            AND_WHEN( "it is rotated again" )
            {
                const auto nextPath = capture.log.rotate( PartialLine::WriteOut, error );

                THEN( "the files are numbered on" )
                {
                    REQUIRE( QFileInfo( nextPath ).fileName() == "serial__dev_ttyUSB0_2.log" );
                    REQUIRE( capture.log.rotationCount() == 2 );
                }
            }
        }

        WHEN( "it is rotated keeping the partial line, and the device ends its line" )
        {
            const auto newPath = capture.log.rotate( PartialLine::Keep, error );
            REQUIRE( capture.log.receive( "rd\n" ) );

            THEN( "the line goes whole to the new file" )
            {
                REQUIRE( readFile( newPath ) == "third\n" );
                REQUIRE( readFile( oldPath ) == "first\nsecond\n" );
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
    }

    GIVEN( "a capture into a generated file in its log directory" )
    {
        QTemporaryDir logDir;
        const auto savePath = CaptureLog::generateLogPath( logDir.path(), kPort );
        FileCapture capture( savePath );
        REQUIRE( capture.opened );
        REQUIRE( capture.log.receive( "first\nsecond\n" ) );

        WHEN( "it is rotated within the same second" )
        {
            QString error;
            const auto newPath = capture.log.rotate( PartialLine::WriteOut, error );

            THEN( "the capture goes on in a different file of that directory" )
            {
                REQUIRE_FALSE( newPath.isEmpty() );
                REQUIRE( newPath != savePath );
                REQUIRE( QFileInfo( newPath ).absolutePath()
                         == QFileInfo( savePath ).absolutePath() );
                REQUIRE( capture.log.path() == newPath );
                REQUIRE( capture.log.receive( "third\n" ) );
                REQUIRE( readFile( newPath ) == "third\n" );
            }

            THEN( "the old file keeps its content" )
            {
                REQUIRE( readFile( savePath ) == "first\nsecond\n" );
            }
        }
    }

    GIVEN( "a capture into a device, not a file of its own" )
    {
        QBuffer file;
        file.open( QIODevice::WriteOnly );
        CaptureLog log( false );
        log.start( &file );
        REQUIRE( log.receive( "first\n" ) );

        WHEN( "it is rotated" )
        {
            QString error;
            const auto newPath = log.rotate( PartialLine::WriteOut, error );

            THEN( "nothing moves" )
            {
                REQUIRE( newPath.isEmpty() );
                REQUIRE( log.rotationCount() == 0 );
                REQUIRE( log.receive( "second\n" ) );
                REQUIRE( file.data() == "first\nsecond\n" );
            }
        }
    }
}

SCENARIO( "a rotation that cannot create the new file", "[capturelog][rotation]" )
{
    if ( !ReadOnlyDir::isEnforced() ) {
        WARN( "File permissions are not enforced (running as root?); skipped." );
        return;
    }

    GIVEN( "a capture whose log directory no longer accepts new files" )
    {
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        FileCapture capture( savePath );
        REQUIRE( capture.opened );
        REQUIRE( capture.log.receive( "first\n" ) );
        QString error;

        WHEN( "it is rotated" )
        {
            QString newPath;
            {
                const ReadOnlyDir readOnly( logDir.path() );
                newPath = capture.log.rotate( PartialLine::WriteOut, error );
            }

            THEN( "it says so, and the capture goes on in the old file" )
            {
                REQUIRE( newPath.isEmpty() );
                REQUIRE( error.contains( "Could not rotate the log" ) );
                REQUIRE( error.contains( "still writing to " + savePath ) );
                REQUIRE( capture.log.isOpen() );
                REQUIRE( capture.log.path() == savePath );
                REQUIRE( capture.log.rotationCount() == 0 );
            }

            THEN( "later lines reach the old file, and its counters go on" )
            {
                REQUIRE( capture.log.receive( "after\n" ) );
                REQUIRE( readFile( savePath ) == "first\nafter\n" );
                REQUIRE( capture.log.lineCount() == 2 );
                REQUIRE( capture.log.fileSize() == 12 );
            }
        }

        WHEN( "it is rotated, and the old file cannot be reopened either" )
        {
            QString newPath;
            {
                QFile::setPermissions( savePath, QFileDevice::ReadOwner );
                const ReadOnlyDir readOnly( logDir.path() );
                newPath = capture.log.rotate( PartialLine::WriteOut, error );
            }

            THEN( "it says so, and the capture has ended instead of going on without a file" )
            {
                REQUIRE( newPath.isEmpty() );
                REQUIRE( error.contains( "nor reopen " + savePath ) );
                REQUIRE( error.contains( "capture stopped" ) );
                REQUIRE_FALSE( capture.log.isOpen() );
            }

            THEN( "what was written before is kept" )
            {
                REQUIRE( readFile( savePath ) == "first\n" );
            }
        }
    }
}

SCENARIO( "a file that reaches the size limit asks to be rotated", "[capturelog][sizelimit]" )
{
    GIVEN( "a capture whose file may hold 16 bytes" )
    {
        FileCapture capture( {}, 16 );
        REQUIRE( capture.opened );
        const auto& requests = capture.requests;
        QString error;

        THEN( "a rotation is due at the limit" )
        {
            REQUIRE( capture.log.nextRotationSize() == 16 );
        }

        WHEN( "the file stays below it" )
        {
            REQUIRE( capture.log.receive( "0123456789\n" ) );

            THEN( "no rotation is asked for" )
            {
                REQUIRE( requests.isEmpty() );
                REQUIRE_FALSE( capture.log.hasReachedSizeLimit() );
            }
        }

        WHEN( "one read reaches it, with a line after the one that does" )
        {
            REQUIRE( capture.log.receive( "0123456789\nabcdefghij\nmore\n" ) );

            THEN( "a rotation is asked for once, after the line that reached it" )
            {
                REQUIRE( requests == QList<qint64>{ 22 } );
                REQUIRE( capture.log.hasReachedSizeLimit() );
            }

            THEN( "the next request is due the limit past the end of that line" )
            {
                REQUIRE( capture.log.nextRotationSize() == 22 + 16 );
            }

            AND_WHEN( "the file grows on, by less than the limit, while nobody rotates it" )
            {
                REQUIRE( capture.log.receive( "more\n" ) );

                THEN( "it is not asked for again" )
                {
                    REQUIRE( requests.size() == 1 );
                }
            }

            AND_WHEN( "it is rotated in the middle of a line" )
            {
                REQUIRE( capture.log.receive( "par" ) );
                const auto newPath = capture.log.rotate( PartialLine::Keep, error );
                REQUIRE( capture.log.receive( "tial\n" ) );

                THEN( "the new file starts below the limit, and is due at it" )
                {
                    REQUIRE( readFile( newPath ) == "partial\n" );
                    REQUIRE_FALSE( capture.log.hasReachedSizeLimit() );
                    REQUIRE( capture.log.nextRotationSize() == 16 );
                }

                AND_WHEN( "the new file reaches it" )
                {
                    REQUIRE( capture.log.receive( "0123456789\n" ) );

                    THEN( "a rotation is asked for again" )
                    {
                        REQUIRE( requests == QList<qint64>{ 22, 19 } );
                    }
                }
            }
        }

        WHEN( "one read grows the file by twice the limit, line after line" )
        {
            REQUIRE( capture.log.receive( "0123456789\nabcdefghij\n0123456789\nabcdefghij\n" ) );

            THEN( "a rotation is asked for at each line that passed the next due size" )
            {
                REQUIRE( requests == QList<qint64>{ 22, 44 } );
                REQUIRE( capture.log.nextRotationSize() == 44 + 16 );
            }
        }

        WHEN( "a command sent reaches it" )
        {
            REQUIRE( capture.log.send( "a command longer than the limit" ) );

            THEN( "a rotation is asked for" )
            {
                REQUIRE( requests == QList<qint64>{ 37 } );
            }
        }
    }

    GIVEN( "a capture at the limit whose rotation fails" )
    {
        if ( !ReadOnlyDir::isEnforced() ) {
            WARN( "File permissions are not enforced (running as root?); skipped." );
            return;
        }

        QTemporaryDir logDir;
        FileCapture capture( logDir.filePath( "capture.log" ), 16 );
        REQUIRE( capture.opened );
        const auto& requests = capture.requests;
        REQUIRE( capture.log.receive( "0123456789\nabcdefghij\n" ) );
        REQUIRE( requests.size() == 1 );
        QString error;
        {
            const ReadOnlyDir readOnly( logDir.path() );
            REQUIRE( capture.log.rotate( PartialLine::Keep, error ).isEmpty() );
        }

        THEN( "the next request is due once the file has grown by the limit again" )
        {
            REQUIRE( capture.log.rotationCount() == 0 );
            REQUIRE( capture.log.nextRotationSize() == capture.log.fileSize() + 16 );
        }

        WHEN( "the file grows by less than the limit again" )
        {
            REQUIRE( capture.log.receive( "short\n" ) );

            THEN( "the failed rotation is not asked for on every line" )
            {
                REQUIRE( requests.size() == 1 );
            }

            AND_WHEN( "it has grown by the limit since the failed rotation" )
            {
                REQUIRE( capture.log.receive( "0123456789\n" ) );

                THEN( "a rotation is asked for again" )
                {
                    REQUIRE( requests == QList<qint64>{ 22, 39 } );
                    REQUIRE( capture.log.nextRotationSize() == 39 + 16 );
                }
            }
        }
    }

    GIVEN( "a capture without a size limit" )
    {
        FileCapture capture( {}, 0 );
        REQUIRE( capture.opened );
        const auto& requests = capture.requests;

        WHEN( "it writes a lot" )
        {
            REQUIRE( capture.log.receive( QByteArray( 900, 'x' ) + "\n" ) );

            THEN( "no rotation is asked for, nor due" )
            {
                REQUIRE( requests.isEmpty() );
                REQUIRE_FALSE( capture.log.hasReachedSizeLimit() );
                REQUIRE( capture.log.nextRotationSize() == 0 );
            }
        }
    }

    GIVEN( "a save path that already holds more than the limit" )
    {
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        writeFile( savePath, QByteArray( 64, 'x' ) + "\n" );
        FileCapture capture( savePath, 16 );
        REQUIRE( capture.opened );

        WHEN( "the capture appends its first line" )
        {
            REQUIRE( capture.log.receive( "first\n" ) );

            THEN( "a rotation is asked for" )
            {
                REQUIRE( capture.requests == QList<qint64>{ 71 } );
            }
        }
    }
}
