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
 * @file serialprocess_test.cpp
 * @brief BDD tests for SerialProcess instance behaviour.
 *
 * Tests basic construction, property accessors and the default
 * configuration without requiring real serial hardware.  The
 * scenarios that need an open port use a pseudo-terminal as the device
 * (see pseudoterminal.h) and only run on Unix.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "filesizelimit.h"
#include "plugin.h"
#include "pseudoterminal.h"
#include "readonlydir.h"
#include "serialprocess.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <sys/types.h>
#endif

using serial_monitor::SerialConfig;
using serial_monitor::SerialProcess;
using serial_test::FakeHost;
using serial_test::ReadOnlyDir;
using serial_test::waitFor;

namespace {

/// A port name that exists on no system.
const QString kMissingPort = "logsquirl-test-no-such-port";

#ifdef Q_OS_UNIX
QByteArray readFile( const QString& path )
{
    QFile file( path );
    return file.open( QIODevice::ReadOnly ) ? file.readAll() : QByteArray();
}
#endif

SerialConfig configFor( const QString& portName )
{
    SerialConfig cfg;
    cfg.portName = portName;
    cfg.timestamps = false; // keep the file content predictable
    return cfg;
}

} // namespace

SCENARIO( "SerialProcess construction and properties", "[serialprocess]" )
{
    GIVEN( "a freshly constructed SerialProcess with default config" )
    {
        SerialConfig cfg;
        cfg.portName = "/dev/ttyUSB0";
        cfg.baudRate = 115200;

        FakeHost host;
        SerialProcess proc( host, cfg );

        THEN( "the port name matches the constructor argument" )
        {
            REQUIRE( proc.portName() == "/dev/ttyUSB0" );
        }

        THEN( "it is not running initially" )
        {
            REQUIRE_FALSE( proc.isRunning() );
        }

        THEN( "line count starts at zero" )
        {
            REQUIRE( proc.lineCount() == 0 );
        }
    }
}

SCENARIO( "a configuration has sensible defaults", "[serialprocess]" )
{
    GIVEN( "a configuration as constructed" )
    {
        const SerialConfig cfg;

        THEN( "the baud rate is 115200" )
        {
            REQUIRE( cfg.baudRate == 115200 );
        }

        THEN( "data bits is 8" )
        {
            REQUIRE( cfg.dataBits == QSerialPort::Data8 );
        }

        THEN( "stop bits is 1" )
        {
            REQUIRE( cfg.stopBits == QSerialPort::OneStop );
        }

        THEN( "parity is None" )
        {
            REQUIRE( cfg.parity == QSerialPort::NoParity );
        }

        THEN( "flow control is None" )
        {
            REQUIRE( cfg.flowControl == QSerialPort::NoFlowControl );
        }

        THEN( "timestamps are enabled by default" )
        {
            REQUIRE( cfg.timestamps == true );
        }

        THEN( "a log file is rotated at the default size" )
        {
            REQUIRE( cfg.maxFileBytes == serial_monitor::kDefaultMaxFileMiB * 1024 * 1024 );
        }
    }
}

SCENARIO( "a session that fails to start leaves the telling to its owner", "[serialprocess]" )
{
    GIVEN( "a SerialProcess for a port that does not exist" )
    {
        FakeHost host;
        SerialProcess proc( host, configFor( kMissingPort ) );

        WHEN( "it fails to start" )
        {
            REQUIRE_FALSE( proc.start() );

            THEN( "it neither logs nor notifies on the host: it emitted the error" )
            {
                REQUIRE( host.logs.isEmpty() );
                REQUIRE( host.notifications.isEmpty() );
            }
        }
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "a session logs to the host it is handed, and writes below its temporary root",
          "[serialprocess]" )
{
    GIVEN( "a SerialProcess on a device, without a save path" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        SerialProcess proc( host, configFor( device.devicePath() ) );

        WHEN( "it starts and stops" )
        {
            REQUIRE( proc.start() );
            const auto path = proc.tempFilePath();
            proc.stop();

            THEN( "its log file is below the host's temporary root" )
            {
                const auto root = QFileInfo( host.tempRoot() ).canonicalFilePath();
                REQUIRE_FALSE( root.isEmpty() );
                REQUIRE( QFileInfo( path ).canonicalFilePath().startsWith( root + "/" ) );
            }

            THEN( "the host's log says the port was opened and closed; nothing is notified" )
            {
                REQUIRE( host.logs.filter( "Opened " + device.devicePath() ).size() == 1 );
                REQUIRE( host.logs.filter( "Closed " + device.devicePath() ).size() == 1 );
                REQUIRE( host.notifications.isEmpty() );
            }
        }
    }
}
#endif

SCENARIO( "rotateLog does nothing while no session runs", "[serialprocess]" )
{
    GIVEN( "a SerialProcess that has not been started" )
    {
        FakeHost host;
        SerialConfig cfg;
        cfg.portName = "/dev/ttyUSB0";
        SerialProcess proc( host, cfg );

        WHEN( "rotateLog is called" )
        {
            const auto result = proc.rotateLog();

            THEN( "it returns an empty path and the line count stays at 0" )
            {
                REQUIRE( result.isEmpty() );
                REQUIRE( proc.lineCount() == 0 );
            }
        }
    }
}

SCENARIO( "start reports whether the port could be opened", "[serialprocess]" )
{
    GIVEN( "a port that does not exist" )
    {
        FakeHost host;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );

        QStringList errors;
        SerialProcess proc( host, configFor( kMissingPort ), savePath );
        QObject::connect( &proc, &SerialProcess::errorOccurred,
                          [ &errors ]( const QString& message ) { errors << message; } );

        WHEN( "starting the session" )
        {
            const auto started = proc.start();

            THEN( "start() fails and the session is not running" )
            {
                REQUIRE_FALSE( started );
                REQUIRE_FALSE( proc.isRunning() );
            }

            THEN( "the failure is reported exactly once" )
            {
                REQUIRE( errors.size() == 1 );
            }

            THEN( "no empty log file is left behind" )
            {
                REQUIRE_FALSE( QFileInfo::exists( savePath ) );
                REQUIRE( proc.tempFilePath().isEmpty() );
            }
        }
    }

    GIVEN( "a port that does not exist and a save path that already holds a capture" )
    {
        FakeHost host;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        {
            QFile existing( savePath );
            REQUIRE( existing.open( QIODevice::WriteOnly ) );
            existing.write( "earlier capture\n" );
        }

        SerialProcess proc( host, configFor( kMissingPort ), savePath );

        WHEN( "starting the session fails" )
        {
            REQUIRE_FALSE( proc.start() );

            THEN( "the earlier capture is kept" )
            {
                REQUIRE( QFileInfo::exists( savePath ) );
            }
        }
    }

#ifdef Q_OS_UNIX
    GIVEN( "a device on a working port" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;

        SerialProcess proc( host, configFor( device.devicePath() ) );

        WHEN( "starting the session" )
        {
            const auto started = proc.start();

            THEN( "start() succeeds and the device's output reaches the log file" )
            {
                REQUIRE( started );
                REQUIRE( proc.isRunning() );
                REQUIRE( device.send( "first\r\nsecond\n" ) );
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 2; } ) );
                REQUIRE( readFile( proc.tempFilePath() ) == "first\nsecond\n" );
            }
        }
    }

    GIVEN( "a device that boots with NUL padding and baud-mismatch garbage" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;

        SerialProcess proc( host, configFor( device.devicePath() ) );

        WHEN( "it sends them before its first line of text" )
        {
            REQUIRE( proc.start() );
            REQUIRE( device.send( QByteArray( "\0\0\0\x02\xF8\x1B\x7F\x80ready\n", 14 ) ) );

            THEN( "the control bytes are written as visible escapes, the text as it came" )
            {
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );
                REQUIRE( readFile( proc.tempFilePath() )
                         == "\\x00\\x00\\x00\\x02\xF8\\x1B\x7F\x80ready\n" );
            }
        }
    }

    GIVEN( "a device that has sent part of a line" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;

        SerialProcess proc( host, configFor( device.devicePath() ) );
        REQUIRE( proc.start() );
        // Sent with a complete line, so the session has read the part once
        // it has written that line.
        REQUIRE( device.send( "boot\nfoo" ) );
        REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );

        WHEN( "a command is sent, and the device then ends its line" )
        {
            REQUIRE( proc.sendData( "cmd" ) );
            REQUIRE( device.send( "bar\n" ) );

            THEN( "the part received before the command stays before it in the log" )
            {
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 4; } ) );
                REQUIRE( readFile( proc.tempFilePath() ) == "boot\nfoo\n[TX] cmd\nbar\n" );
            }
        }
    }

    GIVEN( "a device that has sent part of a line, ending inside a character" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;

        SerialProcess proc( host, configFor( device.devicePath() ) );
        REQUIRE( proc.start() );
        REQUIRE( device.send( "boot\n25 \xC2" ) ); // the first byte of a degree sign
        REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );

        WHEN( "a command is sent, and the device then ends its line" )
        {
            REQUIRE( proc.sendData( "cmd" ) );
            REQUIRE( device.send( "\xB0"
                                  "C\n" ) );

            THEN( "the character is kept whole, after the command" )
            {
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 4; } ) );
                REQUIRE( readFile( proc.tempFilePath() )
                         == "boot\n25 \n[TX] cmd\n\xC2\xB0"
                            "C\n" );
            }
        }
    }

    GIVEN( "a device that ends its lines with a lone CR" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;

        SerialProcess proc( host, configFor( device.devicePath() ) );

        WHEN( "it sends a line and then goes quiet" )
        {
            REQUIRE( proc.start() );
            REQUIRE( device.send( "first\r" ) );

            THEN( "the line is written right away, not when the next one arrives" )
            {
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );
                REQUIRE( readFile( proc.tempFilePath() ) == "first\n" );
            }

            AND_WHEN( "the LF of a CRLF pair follows in a later read" )
            {
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );
                REQUIRE( device.send( "\nsecond\n" ) );

                THEN( "it does not add an empty line" )
                {
                    REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 2; } ) );
                    REQUIRE( readFile( proc.tempFilePath() ) == "first\nsecond\n" );
                }
            }
        }
    }

    GIVEN( "a device and a save path that already holds a capture" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        {
            QFile existing( savePath );
            REQUIRE( existing.open( QIODevice::WriteOnly ) );
            existing.write( "earlier capture\n" );
        }

        SerialProcess proc( host, configFor( device.devicePath() ), savePath );

        WHEN( "the session runs" )
        {
            REQUIRE( proc.start() );
            REQUIRE( device.send( "first\n" ) );
            REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );

            THEN( "the new output is appended to the earlier capture" )
            {
                REQUIRE( readFile( savePath ) == "earlier capture\nfirst\n" );
            }
        }
    }

    GIVEN( "a device and a fixed save path" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );

        WHEN( "a session is stopped and a new one started with the same path" )
        {
            SerialProcess first( host, configFor( device.devicePath() ), savePath );
            REQUIRE( first.start() );
            REQUIRE( device.send( "one\n" ) );
            REQUIRE( waitFor( [ &first ]() { return first.lineCount() == 1; } ) );
            first.stop();

            SerialProcess second( host, configFor( device.devicePath() ), savePath );
            REQUIRE( second.start() );
            REQUIRE( device.send( "two\n" ) );
            REQUIRE( waitFor( [ &second ]() { return second.lineCount() == 1; } ) );
            second.stop();

            THEN( "the file holds both captures" )
            {
                REQUIRE( readFile( savePath ) == "one\ntwo\n" );
            }
        }
    }
#endif
}

SCENARIO( "a session that has not started has counted nothing", "[serialprocess][counters]" )
{
    GIVEN( "a SerialProcess that has not been started" )
    {
        FakeHost host;
        SerialProcess proc( host, configFor( kMissingPort ) );

        THEN( "its counters are zero and it has no start time" )
        {
            REQUIRE( proc.totalLineCount() == 0 );
            REQUIRE( proc.receivedBytes() == 0 );
            REQUIRE( proc.fileSize() == 0 );
            REQUIRE( proc.rotationCount() == 0 );
            REQUIRE( proc.nextRotationSize() == 0 );
            REQUIRE_FALSE( proc.startTime().isValid() );
        }
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "a session's counters follow what it captured", "[serialprocess][counters]" )
{
    GIVEN( "a running session whose log file is rotated at 1000 bytes" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        auto cfg = configFor( device.devicePath() );
        cfg.maxFileBytes = 1000;
        SerialProcess proc( host, cfg );
        const auto beforeStart = QDateTime::currentDateTime();
        REQUIRE( proc.start() );
        const auto afterStart = QDateTime::currentDateTime();

        THEN( "it knows when it started, and when its file is rotated" )
        {
            REQUIRE( proc.startTime() >= beforeStart );
            REQUIRE( proc.startTime() <= afterStart );
            REQUIRE( proc.nextRotationSize() == 1000 );
            REQUIRE( proc.rotationCount() == 0 );
        }

        WHEN( "the device sends two lines and part of a third" )
        {
            REQUIRE( device.send( "first\r\nsecond\nthi" ) );
            REQUIRE( waitFor( [ &proc ]() { return proc.receivedBytes() == 17; } ) );

            THEN( "the bytes received, the lines written and the file's size are counted" )
            {
                REQUIRE( proc.receivedBytes() == 17 );
                REQUIRE( proc.totalLineCount() == 2 );
                REQUIRE( proc.fileSize() == 13 ); // "first\nsecond\n"
            }

            AND_WHEN( "the log is rotated, and the device ends its line" )
            {
                REQUIRE_FALSE( proc.rotateLog( SerialProcess::PartialLine::Keep ).isEmpty() );
                REQUIRE( device.send( "rd\n" ) );
                REQUIRE( waitFor( [ &proc ]() { return proc.totalLineCount() == 3; } ) );

                THEN( "the rotation is counted, and the new file's size starts again" )
                {
                    REQUIRE( proc.rotationCount() == 1 );
                    REQUIRE( proc.fileSize() == 6 ); // "third\n"
                    REQUIRE( proc.nextRotationSize() == 1000 );
                }

                THEN( "the lines and bytes of the whole session go on" )
                {
                    REQUIRE( proc.receivedBytes() == 20 );
                    REQUIRE( proc.totalLineCount() == 3 );
                    REQUIRE( proc.lineCount() == 1 );
                }
            }
        }
    }

    GIVEN( "a save path that already holds a capture" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        {
            QFile earlier( savePath );
            REQUIRE( earlier.open( QIODevice::WriteOnly ) );
            earlier.write( "earlier\n" );
        }
        SerialProcess proc( host, configFor( device.devicePath() ), savePath );
        REQUIRE( proc.start() );

        WHEN( "the capture appends a line" )
        {
            REQUIRE( device.send( "new\n" ) );
            REQUIRE( waitFor( [ &proc ]() { return proc.totalLineCount() == 1; } ) );

            THEN( "the file's size counts what it held, the bytes received do not" )
            {
                REQUIRE( proc.fileSize() == 12 );
                REQUIRE( proc.receivedBytes() == 4 );
            }
        }
    }

    GIVEN( "a running session without a size limit" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        auto cfg = configFor( device.devicePath() );
        cfg.maxFileBytes = 0;
        SerialProcess proc( host, cfg );
        REQUIRE( proc.start() );

        THEN( "no rotation is due" )
        {
            REQUIRE( proc.nextRotationSize() == 0 );
        }
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "rotateLog moves the capture to a new file", "[serialprocess]" )
{
    GIVEN( "a running session writing to a generated file in the log directory" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto savePath = SerialProcess::generateLogPath( logDir.path(), device.devicePath() );

        SerialProcess proc( host, configFor( device.devicePath() ), savePath );
        REQUIRE( proc.start() );
        REQUIRE( device.send( "first\nsecond\n" ) );
        REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 2; } ) );

        WHEN( "rotating within the same second" )
        {
            const auto newPath = proc.rotateLog();

            THEN( "the capture continues in a different file" )
            {
                REQUIRE_FALSE( newPath.isEmpty() );
                REQUIRE( newPath != savePath );
                REQUIRE( proc.tempFilePath() == newPath );
                REQUIRE( QFileInfo::exists( newPath ) );
            }

            THEN( "the old file keeps its content" )
            {
                REQUIRE( readFile( savePath ) == "first\nsecond\n" );
            }
        }
    }

    GIVEN( "a running session writing to a temporary file" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;

        SerialProcess proc( host, configFor( device.devicePath() ) );
        REQUIRE( proc.start() );
        REQUIRE( device.send( "first\nsecond\n" ) );
        REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 2; } ) );
        const auto oldPath = proc.tempFilePath();

        WHEN( "rotating" )
        {
            const auto newPath = proc.rotateLog();

            THEN( "the capture continues in a different file" )
            {
                REQUIRE_FALSE( newPath.isEmpty() );
                REQUIRE( newPath != oldPath );
            }

            THEN( "the old file keeps its content" )
            {
                REQUIRE( readFile( oldPath ) == "first\nsecond\n" );
            }
        }
    }

    GIVEN( "a running session whose log directory no longer accepts new files" )
    {
        if ( !ReadOnlyDir::isEnforced() ) {
            WARN( "File permissions are not enforced (running as root?); skipped." );
            return;
        }

        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );

        QStringList errors;
        SerialProcess proc( host, configFor( device.devicePath() ), savePath );
        QObject::connect( &proc, &SerialProcess::errorOccurred,
                          [ &errors ]( const QString& message ) { errors << message; } );
        REQUIRE( proc.start() );
        REQUIRE( device.send( "first\n" ) );
        REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );

        WHEN( "rotating fails" )
        {
            QString newPath;
            {
                ReadOnlyDir readOnly( logDir.path() );
                newPath = proc.rotateLog();
            }
            REQUIRE( device.send( "after\n" ) );

            THEN( "the failure is reported once and the session keeps running" )
            {
                REQUIRE( newPath.isEmpty() );
                REQUIRE( errors.size() == 1 );
                REQUIRE( proc.isRunning() );
            }

            THEN( "later output still reaches the old file" )
            {
                REQUIRE( proc.tempFilePath() == savePath );
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 2; } ) );
                REQUIRE( readFile( savePath ) == "first\nafter\n" );
            }
        }

        AND_WHEN( "rotating fails and the old file cannot be reopened either" )
        {
            int finishedCount = 0;
            QObject::connect( &proc, &SerialProcess::finished,
                              [ &finishedCount ]() { ++finishedCount; } );
            QString newPath;
            {
                QFile::setPermissions( savePath, QFileDevice::ReadOwner );
                ReadOnlyDir readOnly( logDir.path() );
                newPath = proc.rotateLog();
            }

            THEN( "the session stops instead of running without a file" )
            {
                REQUIRE( newPath.isEmpty() );
                REQUIRE( errors.size() == 1 );
                REQUIRE_FALSE( proc.isRunning() );
                REQUIRE( finishedCount == 1 );
            }
        }
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "a log file that reaches its size limit asks to be rotated", "[serialprocess]" )
{
    GIVEN( "a running session whose log file may hold 16 bytes" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        auto cfg = configFor( device.devicePath() );
        cfg.maxFileBytes = 16;
        SerialProcess proc( host, cfg );
        int requests = 0;
        QObject::connect( &proc, &SerialProcess::sizeLimitReached,
                          [ &requests ]() { ++requests; } );
        REQUIRE( proc.start() );
        const auto oldPath = proc.tempFilePath();

        WHEN( "the file stays below it" )
        {
            REQUIRE( device.send( "0123456789\n" ) );
            REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );
            serial_test::processEventsFor( 100 );

            THEN( "no rotation is asked for" )
            {
                REQUIRE( requests == 0 );
            }
        }

        WHEN( "the file reaches it, and the user rotates it before the request is handled" )
        {
            REQUIRE( proc.sendData( "a command longer than the limit" ) );
            REQUIRE_FALSE( proc.rotateLog().isEmpty() );
            serial_test::processEventsFor( 100 );

            THEN( "no rotation is asked for: the new file is empty" )
            {
                REQUIRE( requests == 0 );
            }
        }

        WHEN( "the file reaches it, and grows on while nobody rotates it" )
        {
            REQUIRE( device.send( "0123456789\nabcdefghij\n" ) );
            REQUIRE( waitFor( [ &requests ]() { return requests == 1; } ) );
            REQUIRE( device.send( "more\nand more\n" ) );
            REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 4; } ) );
            serial_test::processEventsFor( 100 );

            THEN( "a rotation is asked for once" )
            {
                REQUIRE( requests == 1 );
            }

            AND_WHEN( "it is rotated in the middle of a line" )
            {
                REQUIRE( device.send( "par" ) );
                serial_test::processEventsFor( 100 );
                const auto newPath = proc.rotateLog( SerialProcess::PartialLine::Keep );
                REQUIRE( device.send( "tial\n" ) );
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );

                THEN( "the line goes whole to the new file" )
                {
                    REQUIRE( readFile( newPath ) == "partial\n" );
                    REQUIRE( readFile( oldPath ) == "0123456789\nabcdefghij\nmore\nand more\n" );
                }

                AND_WHEN( "the new file reaches it" )
                {
                    REQUIRE( device.send( "0123456789\nabcdefghij\n" ) );

                    THEN( "a rotation is asked for again" )
                    {
                        REQUIRE( waitFor( [ &requests ]() { return requests == 2; } ) );
                    }
                }
            }
        }
    }

    GIVEN( "a session at the limit whose log directory no longer accepts new files" )
    {
        if ( !ReadOnlyDir::isEnforced() ) {
            WARN( "File permissions are not enforced (running as root?); skipped." );
            return;
        }

        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        auto cfg = configFor( device.devicePath() );
        cfg.maxFileBytes = 16;
        SerialProcess proc( host, cfg, logDir.filePath( "capture.log" ) );
        int requests = 0;
        QObject::connect( &proc, &SerialProcess::sizeLimitReached, [ & ]() {
            ++requests;
            const ReadOnlyDir readOnly( logDir.path() );
            proc.rotateLog( SerialProcess::PartialLine::Keep );
        } );
        REQUIRE( proc.start() );
        REQUIRE( device.send( "0123456789\nabcdefghij\n" ) );
        REQUIRE( waitFor( [ &requests ]() { return requests == 1; } ) );

        THEN( "the next rotation is due once the file has grown by the limit again" )
        {
            REQUIRE( proc.rotationCount() == 0 );
            REQUIRE( proc.nextRotationSize() == proc.fileSize() + 16 );
        }

        WHEN( "the file grows by less than the limit again" )
        {
            REQUIRE( device.send( "short\n" ) );
            REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 3; } ) );
            serial_test::processEventsFor( 100 );

            THEN( "the failed rotation is not retried on every line" )
            {
                REQUIRE( requests == 1 );
            }

            AND_WHEN( "it has grown by the limit since the failed rotation" )
            {
                REQUIRE( device.send( "0123456789\n" ) );

                THEN( "a rotation is asked for again" )
                {
                    REQUIRE( waitFor( [ &requests ]() { return requests == 2; } ) );
                }
            }
        }
    }

    GIVEN( "a running session without a size limit" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        auto cfg = configFor( device.devicePath() );
        cfg.maxFileBytes = 0;
        SerialProcess proc( host, cfg );
        int requests = 0;
        QObject::connect( &proc, &SerialProcess::sizeLimitReached,
                          [ &requests ]() { ++requests; } );
        REQUIRE( proc.start() );

        WHEN( "the device sends a lot" )
        {
            REQUIRE( device.send( QByteArray( 900, 'x' ) + "\n" ) );
            REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );
            serial_test::processEventsFor( 100 );

            THEN( "no rotation is asked for" )
            {
                REQUIRE( requests == 0 );
            }
        }
    }

    GIVEN( "a save path that already holds more than the limit" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        {
            QFile earlier( savePath );
            REQUIRE( earlier.open( QIODevice::WriteOnly ) );
            earlier.write( QByteArray( 64, 'x' ) + "\n" );
        }
        auto cfg = configFor( device.devicePath() );
        cfg.maxFileBytes = 16;
        SerialProcess proc( host, cfg, savePath );
        int requests = 0;
        QObject::connect( &proc, &SerialProcess::sizeLimitReached,
                          [ &requests ]() { ++requests; } );
        REQUIRE( proc.start() );

        WHEN( "the capture appends its first line" )
        {
            REQUIRE( device.send( "first\n" ) );

            THEN( "a rotation is asked for" )
            {
                REQUIRE( waitFor( [ &requests ]() { return requests == 1; } ) );
            }
        }
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "destroying a running session does not call back its owner", "[serialprocess]" )
{
    GIVEN( "a running session with listeners" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        int signalCount = 0;
        auto* proc = new SerialProcess( host, configFor( device.devicePath() ) );
        QObject::connect( proc, &SerialProcess::finished, [ &signalCount ]() { ++signalCount; } );
        QObject::connect( proc, &SerialProcess::errorOccurred,
                          [ &signalCount ]( const QString& ) { ++signalCount; } );
        REQUIRE( proc->start() );

        WHEN( "it is destroyed, as when its owner's children are deleted" )
        {
            delete proc;

            THEN( "no signal is emitted into the (possibly half-destroyed) owner" )
            {
                REQUIRE( signalCount == 0 );
            }
        }
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "a log file that cannot be written stops the session and says why", "[serialprocess]" )
{
    GIVEN( "a running session whose log file may grow by a few bytes only" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        SerialProcess proc( host, configFor( device.devicePath() ), savePath );
        QStringList errors;
        QObject::connect( &proc, &SerialProcess::errorOccurred,
                          [ &errors ]( const QString& message ) { errors << message; } );
        int finishedCount = 0;
        QObject::connect( &proc, &SerialProcess::finished,
                          [ &finishedCount ]() { ++finishedCount; } );
        REQUIRE( proc.start() );
        serial_test::FileSizeLimit limit( 8 );
        REQUIRE( limit.isActive() );

        WHEN( "the device sends more than that, line after line" )
        {
            REQUIRE( device.send( "first line\nsecond line\nthird line\n" ) );
            const bool stopped = waitFor( [ &proc ]() { return !proc.isRunning(); } );
            serial_test::processEventsFor( 100 );
            limit.lift();

            THEN( "the session stops, and the failure is reported once, naming the file" )
            {
                REQUIRE( stopped );
                REQUIRE( finishedCount == 1 );
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().contains( "Could not write the log file" ) );
                REQUIRE( errors.first().contains( savePath ) );
            }
        }

        WHEN( "a command is sent" )
        {
            REQUIRE( proc.sendData( "reboot with a long command line" ) );
            const bool stopped = waitFor( [ &proc ]() { return !proc.isRunning(); } );
            limit.lift();

            THEN( "the session stops, and the failure is reported once" )
            {
                REQUIRE( stopped );
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().contains( "Could not write the log file" ) );
            }
        }
    }
}
#endif

SCENARIO( "a typed path is made absolute, and a relative one refused", "[serialprocess]" )
{
    THEN( "~ is the home directory, and the path is cleaned" )
    {
        REQUIRE( SerialProcess::absoluteUserPath( "~" ) == QDir::homePath() );
        REQUIRE( SerialProcess::absoluteUserPath( " ~/serial/x.log " )
                 == QDir::homePath() + "/serial/x.log" );
        const auto root = QDir::rootPath();
        REQUIRE( SerialProcess::absoluteUserPath( root + "a/../b/c.log" ) == root + "b/c.log" );
    }

    THEN( "a relative path, or none, is empty" )
    {
        REQUIRE( SerialProcess::absoluteUserPath( "out.log" ).isEmpty() );
        REQUIRE( SerialProcess::absoluteUserPath( "serial/out.log" ).isEmpty() );
        REQUIRE( SerialProcess::absoluteUserPath( "~user/x.log" ).isEmpty() );
        REQUIRE( SerialProcess::absoluteUserPath( "  " ).isEmpty() );
    }

    GIVEN( "a session with a relative save path" )
    {
        FakeHost host;
        SerialProcess proc( host, configFor( kMissingPort ), "relative-capture.log" );
        QStringList errors;
        QObject::connect( &proc, &SerialProcess::errorOccurred,
                          [ &errors ]( const QString& message ) { errors << message; } );

        WHEN( "it is started" )
        {
            const auto started = proc.start();

            THEN( "it does not start, says why, and creates nothing in the working directory" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().contains( "not a full path" ) );
                REQUIRE_FALSE( QFileInfo::exists( "relative-capture.log" ) );
                REQUIRE( proc.tempFilePath().isEmpty() );
            }
        }
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "only a regular file is opened as the save path", "[serialprocess]" )
{
    GIVEN( "a save path that is a FIFO" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto fifo = logDir.filePath( "capture.log" );
        REQUIRE( ::mkfifo( QFile::encodeName( fifo ).constData(), 0600 ) == 0 );
        SerialProcess proc( host, configFor( device.devicePath() ), fifo );
        QStringList errors;
        QObject::connect( &proc, &SerialProcess::errorOccurred,
                          [ &errors ]( const QString& message ) { errors << message; } );

        WHEN( "it is started" )
        {
            QElapsedTimer timer;
            timer.start();
            const auto started = proc.start();

            THEN( "it is refused at once, without waiting for a reader" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( timer.elapsed() < 2000 );
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().contains( "not a regular file" ) );
                REQUIRE( errors.first().contains( fifo ) );
            }
        }
    }

    GIVEN( "a save path that is a character device" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        SerialProcess proc( host, configFor( device.devicePath() ), "/dev/null" );
        QStringList errors;
        QObject::connect( &proc, &SerialProcess::errorOccurred,
                          [ &errors ]( const QString& message ) { errors << message; } );

        WHEN( "it is started" )
        {
            const auto started = proc.start();

            THEN( "it is refused" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().contains( "not a regular file" ) );
            }
        }
    }

    GIVEN( "a save path that is a symlink to a file that does not exist" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto target = logDir.filePath( "elsewhere.log" );
        const auto link = logDir.filePath( "capture.log" );
        REQUIRE( QFile::link( target, link ) );
        SerialProcess proc( host, configFor( device.devicePath() ), link );

        WHEN( "it is started" )
        {
            const auto started = proc.start();

            THEN( "it is refused, and the link's target is not created" )
            {
                REQUIRE_FALSE( started );
                REQUIRE_FALSE( QFileInfo::exists( target ) );
            }
        }
    }

    GIVEN( "a save path that is a symlink to an existing log file" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto target = logDir.filePath( "elsewhere.log" );
        {
            QFile existing( target );
            REQUIRE( existing.open( QIODevice::WriteOnly ) );
            existing.write( "earlier capture\n" );
        }
        const auto link = logDir.filePath( "capture.log" );
        REQUIRE( QFile::link( target, link ) );
        SerialProcess proc( host, configFor( device.devicePath() ), link );

        WHEN( "it is started" )
        {
            const auto started = proc.start();

            THEN( "it is refused, and the target and the link are left as they were" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( readFile( target ) == "earlier capture\n" );
                REQUIRE( QFileInfo( link ).isSymLink() );
            }
        }
    }

    GIVEN( "a save path that does not exist yet" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        SerialProcess proc( host, configFor( device.devicePath() ), savePath );

        WHEN( "it is started" )
        {
            REQUIRE( proc.start() );

            THEN( "the file it creates is readable by the user only, and named as the log file" )
            {
                struct stat info{};
                REQUIRE( ::stat( QFile::encodeName( savePath ).constData(), &info ) == 0 );
                REQUIRE( ( info.st_mode & 0777 ) == 0600 );
                REQUIRE( proc.tempFilePath() == savePath );
            }

            AND_WHEN( "the device sends a line" )
            {
                REQUIRE( device.send( "first\n" ) );

                THEN( "it reaches the file" )
                {
                    REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );
                    REQUIRE( readFile( savePath ) == "first\n" );
                }
            }

            proc.stop();
        }
    }
}
#endif
