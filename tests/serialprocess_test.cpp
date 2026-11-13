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
 * configuration without requiring real serial hardware.  What the session
 * does when its device, its log file or a rotation fails, and when the
 * file reaches its size limit, is tested with a substitute device in place
 * of the serial port (SubstitutePort), in-process on every platform.  A
 * few end-to-end scenarios use a pseudo-terminal as the device (see
 * pseudoterminal.h) and only run on Unix.
 *
 * The log file itself (which file, regular files only, rotation, the size
 * limit) and what is written to it (framing, escapes, timestamps, [TX]
 * lines, counters) are CaptureLog's, tested in capturelog_test.cpp; the
 * scenarios here check that the session hands its device's bytes to it,
 * and tells what it reports.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "filesizelimit.h"
#include "plugin.h"
#include "pseudoterminal.h"
#include "readonlydir.h"
#include "serialprocess.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstring>
#include <memory>

using serial_monitor::SerialConfig;
using serial_monitor::SerialProcess;
using serial_monitor::TxLineEnding;
using serial_test::FakeHost;
using serial_test::ReadOnlyDir;
using serial_test::waitFor;

namespace {

/// A port name that exists on no system.
const QString kMissingPort = "logsquirl-test-no-such-port";

QByteArray readFile( const QString& path )
{
    QFile file( path );
    return file.open( QIODevice::ReadOnly ) ? file.readAll() : QByteArray();
}

SerialConfig configFor( const QString& portName )
{
    SerialConfig cfg;
    cfg.portName = portName;
    cfg.timestamps = false; // keep the file content predictable
    return cfg;
}

/**
 * A device that stands in for a serial port, played by the test: it
 * delivers what the test sends as the device, keeps what the session sends
 * to it, and fails a read or a write when the test says so.
 */
class SubstitutePort : public QIODevice {
public:
    /// Send @p data as the device.
    void deliver( const QByteArray& data )
    {
        pending_ += data;
        Q_EMIT readyRead();
    }

    /// Fail every read from here on, for @p reason, as a device that is gone.
    void failReads( const QString& reason )
    {
        failReads_ = true;
        setErrorString( reason );
        Q_EMIT readyRead();
    }

    /// Fail every write from here on, for @p reason.
    void failWrites( const QString& reason )
    {
        failWrites_ = true;
        setErrorString( reason );
    }

    /// What the session sent to the device.
    QByteArray sent;

    bool isSequential() const override
    {
        return true;
    }

    qint64 bytesAvailable() const override
    {
        return pending_.size() + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData( char* data, qint64 maxSize ) override
    {
        if ( failReads_ ) {
            return -1;
        }
        const auto size = qMin( maxSize, static_cast<qint64>( pending_.size() ) );
        std::memcpy( data, pending_.constData(), static_cast<size_t>( size ) );
        pending_.remove( 0, size );
        return size;
    }

    qint64 writeData( const char* data, qint64 size ) override
    {
        if ( failWrites_ ) {
            return -1;
        }
        sent.append( data, size );
        return size;
    }

private:
    QByteArray pending_;
    bool failReads_ = false;
    bool failWrites_ = false;
};

/// A session on a SubstitutePort, recording what it reports.
struct SubstituteSession {
    explicit SubstituteSession( const QString& savePath = {}, qint64 maxFileBytes = 0 )
    {
        auto cfg = configFor( "substitute" );
        cfg.maxFileBytes = maxFileBytes;
        auto device = std::make_unique<SubstitutePort>();
        port = device.get();
        proc = std::make_unique<SerialProcess>( host, cfg, savePath, std::move( device ) );
        QObject::connect( proc.get(), &SerialProcess::errorOccurred,
                          [ this ]( const QString& message ) { errors << message; } );
        QObject::connect( proc.get(), &SerialProcess::finished, [ this ]() { ++finishedCount; } );
        QObject::connect( proc.get(), &SerialProcess::sizeLimitReached,
                          [ this ]() { ++sizeLimitRequests; } );
    }

    FakeHost host;
    SubstitutePort* port = nullptr; ///< Owned by proc.
    std::unique_ptr<SerialProcess> proc;
    QStringList errors;
    int finishedCount = 0;
    int sizeLimitRequests = 0;
};

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
            REQUIRE( proc.sendData( "cmd", TxLineEnding::LF ) );
            REQUIRE( device.send( "bar\n" ) );

            THEN( "the command goes to the device with the line ending it was sent with" )
            {
                QByteArray received;
                REQUIRE( waitFor( [ &device, &received ] {
                    received += device.receive();
                    return received == "cmd\n";
                } ) );
            }

            THEN( "the part received before the command stays before it in the log" )
            {
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 4; } ) );
                REQUIRE( readFile( proc.tempFilePath() ) == "boot\nfoo\n[TX] cmd\nbar\n" );
            }
        }
    }

    GIVEN( "a device on a session that timestamps its lines" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        auto cfg = configFor( device.devicePath() );
        cfg.timestamps = true;
        SerialProcess proc( host, cfg );

        WHEN( "the device sends a line" )
        {
            REQUIRE( proc.start() );
            REQUIRE( device.send( "first\n" ) );

            THEN( "it is written after a timestamp" )
            {
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );
                const auto written = readFile( proc.tempFilePath() );
                REQUIRE( written.size() == 32 ); // "[yyyy-MM-dd HH:mm:ss.zzz] first\n"
                REQUIRE( written.startsWith( "[" ) );
                REQUIRE( written.endsWith( "] first\n" ) );
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

SCENARIO( "a session's counters are its capture's", "[serialprocess][counters]" )
{
    GIVEN( "a running session whose log file is rotated at 1000 bytes" )
    {
        SubstituteSession session( {}, 1000 );
        auto& proc = *session.proc;
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
            session.port->deliver( "first\r\nsecond\nthi" );

            THEN( "the bytes received, the lines written and the file's size are counted" )
            {
                REQUIRE( proc.receivedBytes() == 17 );
                REQUIRE( proc.totalLineCount() == 2 );
                REQUIRE( proc.lineCount() == 2 );
                REQUIRE( proc.fileSize() == 13 ); // "first\nsecond\n"
            }

            AND_WHEN( "the log is rotated, and the device ends its line" )
            {
                REQUIRE_FALSE( proc.rotateLog( SerialProcess::PartialLine::Keep ).isEmpty() );
                session.port->deliver( "rd\n" );

                THEN( "the rotation is counted, and the new file's counters start again" )
                {
                    REQUIRE( proc.rotationCount() == 1 );
                    REQUIRE( proc.lineCount() == 1 );
                    REQUIRE( proc.fileSize() == 6 ); // "third\n"
                    REQUIRE( proc.nextRotationSize() == 1000 );
                    REQUIRE( readFile( proc.tempFilePath() ) == "third\n" );
                }

                THEN( "the lines and bytes of the whole session go on" )
                {
                    REQUIRE( proc.receivedBytes() == 20 );
                    REQUIRE( proc.totalLineCount() == 3 );
                }

                THEN( "the host's log says so" )
                {
                    REQUIRE( session.host.logs.filter( "Rotated serial log for substitute" ).size()
                             == 1 );
                }
            }
        }
    }

    GIVEN( "a running session without a size limit" )
    {
        SubstituteSession session( {}, 0 );
        REQUIRE( session.proc->start() );

        THEN( "no rotation is due" )
        {
            REQUIRE( session.proc->nextRotationSize() == 0 );
        }
    }
}

SCENARIO( "a device that fails ends its session", "[serialprocess]" )
{
    GIVEN( "a running session on a device, which has sent a line" )
    {
        SubstituteSession session;
        auto& proc = *session.proc;
        REQUIRE( proc.start() );
        session.port->deliver( "first\n" );

        WHEN( "the device cannot be read any more, as when it is gone" )
        {
            session.port->failReads( "Input/output error" );
            session.port->failReads( "Input/output error" );

            THEN( "the session stops, from the event loop" )
            {
                REQUIRE( proc.isRunning() );
                REQUIRE( waitFor( [ &proc ]() { return !proc.isRunning(); } ) );
                REQUIRE( session.finishedCount == 1 );
            }

            THEN( "it says why, once" )
            {
                serial_test::processEventsFor( 50 );
                REQUIRE( session.errors
                         == QStringList{ "Serial port error on substitute: Input/output error; "
                                         "capture stopped." } );
            }

            THEN( "what was captured before is kept" )
            {
                REQUIRE( waitFor( [ &proc ]() { return !proc.isRunning(); } ) );
                REQUIRE( readFile( proc.tempFilePath() ) == "first\n" );
            }
        }

        WHEN( "the device refuses a command sent to it" )
        {
            session.port->failWrites( "Resource busy" );
            const auto sent = proc.sendData( "reboot", TxLineEnding::LF );

            THEN( "the command is not sent, nor logged, and it says why" )
            {
                REQUIRE_FALSE( sent );
                REQUIRE( session.errors
                         == QStringList{ "Failed to write to substitute: Resource busy" } );
                REQUIRE( readFile( proc.tempFilePath() ) == "first\n" );
            }
        }

        WHEN( "a command is sent" )
        {
            REQUIRE( proc.sendData( "reboot", TxLineEnding::CRLF ) );

            THEN( "it goes to the device with its line ending, and to the log without" )
            {
                REQUIRE( session.port->sent == "reboot\r\n" );
                REQUIRE( readFile( proc.tempFilePath() ) == "first\n[TX] reboot\n" );
            }
        }
    }
}

SCENARIO( "a rotation that fails is told, and ends the session only without a file",
          "[serialprocess]" )
{
    if ( !ReadOnlyDir::isEnforced() ) {
        WARN( "File permissions are not enforced (running as root?); skipped." );
        return;
    }

    GIVEN( "a running session whose log directory no longer accepts new files" )
    {
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        SubstituteSession session( savePath );
        auto& proc = *session.proc;
        REQUIRE( proc.start() );
        session.port->deliver( "first\n" );

        WHEN( "rotating fails" )
        {
            QString newPath;
            {
                const ReadOnlyDir readOnly( logDir.path() );
                newPath = proc.rotateLog();
            }
            session.port->deliver( "after\n" );

            THEN( "the failure is reported once and the session keeps running in the old file" )
            {
                REQUIRE( newPath.isEmpty() );
                REQUIRE( session.errors.size() == 1 );
                REQUIRE( session.errors.first().contains( "still writing to " + savePath ) );
                REQUIRE( proc.isRunning() );
                REQUIRE( proc.tempFilePath() == savePath );
                REQUIRE( readFile( savePath ) == "first\nafter\n" );
            }
        }

        WHEN( "rotating fails and the old file cannot be reopened either" )
        {
            QString newPath;
            {
                QFile::setPermissions( savePath, QFileDevice::ReadOwner );
                const ReadOnlyDir readOnly( logDir.path() );
                newPath = proc.rotateLog();
            }

            THEN( "the session stops instead of running without a file" )
            {
                REQUIRE( newPath.isEmpty() );
                REQUIRE( session.errors.size() == 1 );
                REQUIRE( session.errors.first().contains( "capture stopped" ) );
                REQUIRE_FALSE( proc.isRunning() );
                REQUIRE( session.finishedCount == 1 );
            }
        }
    }
}

SCENARIO( "a log file that reaches its size limit asks to be rotated, from the event loop",
          "[serialprocess]" )
{
    GIVEN( "a running session whose log file may hold 16 bytes" )
    {
        SubstituteSession session( {}, 16 );
        auto& proc = *session.proc;
        REQUIRE( proc.start() );

        WHEN( "the file stays below it" )
        {
            session.port->deliver( "0123456789\n" );
            serial_test::processEventsFor( 50 );

            THEN( "no rotation is asked for" )
            {
                REQUIRE( session.sizeLimitRequests == 0 );
            }
        }

        WHEN( "the file reaches it" )
        {
            session.port->deliver( "0123456789\nabcdefghij\n" );

            THEN( "a rotation is asked for, once the read has been handled" )
            {
                REQUIRE( session.sizeLimitRequests == 0 );
                REQUIRE( waitFor( [ &session ]() { return session.sizeLimitRequests == 1; } ) );
            }
        }

        WHEN( "the file reaches it, and the user rotates it before the request is handled" )
        {
            REQUIRE( proc.sendData( "a command longer than the limit", TxLineEnding::CRLF ) );
            REQUIRE_FALSE( proc.rotateLog().isEmpty() );
            serial_test::processEventsFor( 50 );

            THEN( "no rotation is asked for: the new file is empty" )
            {
                REQUIRE( session.sizeLimitRequests == 0 );
            }
        }

        WHEN( "the file reaches it, and the session stops before the request is handled" )
        {
            session.port->deliver( "0123456789\nabcdefghij\n" );
            proc.stop();
            serial_test::processEventsFor( 50 );

            THEN( "no rotation is asked for" )
            {
                REQUIRE( session.sizeLimitRequests == 0 );
            }
        }
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "rotateLog moves the capture to a new file", "[serialprocess]" )
{
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

            THEN( "the device's output goes to the new file" )
            {
                REQUIRE( device.send( "third\n" ) );
                REQUIRE( waitFor( [ &proc ]() { return proc.lineCount() == 1; } ) );
                REQUIRE( readFile( newPath ) == "third\n" );
            }
        }
    }
}
#endif

SCENARIO( "destroying a running session does not call back its owner", "[serialprocess]" )
{
    GIVEN( "a running session with listeners" )
    {
        SubstituteSession session;
        REQUIRE( session.proc->start() );

        WHEN( "it is destroyed, as when its owner's children are deleted" )
        {
            session.proc.reset();

            THEN( "no signal is emitted into the (possibly half-destroyed) owner" )
            {
                REQUIRE( session.finishedCount == 0 );
                REQUIRE( session.errors.isEmpty() );
            }
        }
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "a log file that cannot be written stops the session and says why", "[serialprocess]" )
{
    GIVEN( "a running session whose log file may grow by a few bytes only" )
    {
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        SubstituteSession session( savePath );
        auto& proc = *session.proc;
        REQUIRE( proc.start() );
        serial_test::FileSizeLimit limit( 8 );
        REQUIRE( limit.isActive() );

        WHEN( "the device sends more than that, line after line" )
        {
            session.port->deliver( "first line\nsecond line\nthird line\n" );
            session.port->deliver( "fourth line\n" );
            const bool stopped = waitFor( [ &proc ]() { return !proc.isRunning(); } );
            serial_test::processEventsFor( 50 );
            limit.lift();

            THEN( "the session stops, and the failure is reported once, naming the file" )
            {
                REQUIRE( stopped );
                REQUIRE( session.finishedCount == 1 );
                REQUIRE( session.errors.size() == 1 );
                REQUIRE( session.errors.first().startsWith( "Could not write the log file "
                                                            + savePath + " (" ) );
                REQUIRE( session.errors.first().endsWith( "); capture stopped." ) );
            }
        }

        WHEN( "a command is sent" )
        {
            REQUIRE( proc.sendData( "reboot with a long command line", TxLineEnding::CRLF ) );
            const bool stopped = waitFor( [ &proc ]() { return !proc.isRunning(); } );
            limit.lift();

            THEN( "the session stops, and the failure is reported once" )
            {
                REQUIRE( stopped );
                REQUIRE( session.errors.size() == 1 );
                REQUIRE( session.errors.first().contains( "Could not write the log file" ) );
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
SCENARIO( "a session whose log file cannot be opened does not start", "[serialprocess]" )
{
    GIVEN( "a session whose save path is not a regular file" )
    {
        SubstituteSession session( "/dev/null" );

        WHEN( "it is started" )
        {
            const auto started = session.proc->start();

            THEN( "it is refused, says why once, and leaves the device closed" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( session.errors.size() == 1 );
                REQUIRE( session.errors.first().contains( "not a regular file" ) );
                REQUIRE_FALSE( session.port->isOpen() );
                REQUIRE_FALSE( session.proc->isRunning() );
            }
        }
    }
}
#endif
