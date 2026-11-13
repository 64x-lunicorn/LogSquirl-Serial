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
 * @file sessions_test.cpp
 * @brief BDD tests for the session bookkeeping in Sessions.
 *
 * Sessions owns the running SerialProcess sessions for both the dialog
 * and the sidebar.  These tests drive it through its public API with a
 * FakeHost and, on Unix, a pseudo-terminal as the device (see
 * pseudoterminal.h).
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "filesizelimit.h"
#include "porterror.h"
#include "pseudoterminal.h"
#include "readonlydir.h"
#include "sessions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using serial_monitor::SerialConfig;
using serial_monitor::Sessions;
using serial_test::FakeHost;
using serial_test::waitFor;

namespace {

SerialConfig configFor( const QString& portName )
{
    SerialConfig cfg;
    cfg.portName = portName;
    return cfg;
}

} // namespace

SCENARIO( "a session is only accepted when its port opens", "[sessions]" )
{
    GIVEN( "a port that does not exist" )
    {
        FakeHost host;
        Sessions sessions( host );
        QTemporaryDir logDir;
        const QString port = "logsquirl-test-no-such-port";

        WHEN( "starting a session with a save path" )
        {
            const auto started
                = sessions.startSession( configFor( port ), logDir.filePath( "capture.log" ) );

            THEN( "the session is rejected" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( sessions.activeSessionCount() == 0 );
                REQUIRE_FALSE( sessions.isSessionActive( port ) );
            }

            THEN( "no tab is opened and no empty file is left behind" )
            {
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE( QDir( logDir.path() ).isEmpty() );
            }

            THEN( "the user is told once why" )
            {
                REQUIRE( host.notifications.size() == 1 );
            }
        }
    }

    GIVEN( "a save path that is not a full path" )
    {
        FakeHost host;
        Sessions sessions( host );
        const QString port = "logsquirl-test-no-such-port";

        WHEN( "starting a session with it" )
        {
            const auto started = sessions.startSession( configFor( port ), "relative-capture.log" );

            THEN( "the session is rejected before anything is written, and the user told why" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE_FALSE( QFileInfo::exists( "relative-capture.log" ) );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "not a full path" ) );
            }
        }
    }

#ifdef Q_OS_UNIX
    GIVEN( "a device and a save path in the home directory, spelled with ~" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        QTemporaryDir home;
        const auto oldHome = qgetenv( "HOME" );
        qputenv( "HOME", QFile::encodeName( home.path() ) );

        WHEN( "starting a session" )
        {
            const auto started
                = sessions.startSession( configFor( device.devicePath() ), "~/serial/capture.log" );
            qputenv( "HOME", oldHome );
            sessions.stopAll();

            THEN( "the capture goes to the home directory, and the tab opens its full path" )
            {
                const auto expected = QDir( home.path() ).filePath( "serial/capture.log" );
                REQUIRE( started );
                REQUIRE( QFileInfo::exists( expected ) );
                REQUIRE( host.openedFiles == QStringList{ expected } );
            }
        }

        qputenv( "HOME", oldHome );
    }

    GIVEN( "a device on a working port" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        QTemporaryDir logDir;

        WHEN( "starting a session" )
        {
            const auto started = sessions.startSession( configFor( device.devicePath() ),
                                                        logDir.filePath( "capture.log" ) );

            THEN( "the session is accepted and its file opened in a tab" )
            {
                REQUIRE( started );
                REQUIRE( sessions.isSessionActive( device.devicePath() ) );
                REQUIRE( host.openedFiles.size() == 1 );
            }

            sessions.stopAll();
        }
    }
#endif
}

#ifdef Q_OS_UNIX
SCENARIO( "two sessions never write to the same file", "[sessions]" )
{
    GIVEN( "a session writing to a save path" )
    {
        FakeHost host;
        serial_test::PseudoTerminal firstDevice;
        serial_test::PseudoTerminal secondDevice;
        Sessions sessions( host );
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        REQUIRE( sessions.startSession( configFor( firstDevice.devicePath() ), savePath ) );
        host.notifications.clear();

        WHEN( "starting a second port with the same save path" )
        {
            const auto started
                = sessions.startSession( configFor( secondDevice.devicePath() ), savePath );

            THEN( "the second session is refused, and the user told why" )
            {
                REQUIRE_FALSE( started );
                REQUIRE_FALSE( sessions.isSessionActive( secondDevice.devicePath() ) );
                REQUIRE( host.notifications.size() == 1 );
            }
        }

        sessions.stopAll();
    }

    GIVEN( "a session whose save file has been deleted while it runs" )
    {
        FakeHost host;
        serial_test::PseudoTerminal firstDevice;
        serial_test::PseudoTerminal secondDevice;
        Sessions sessions( host );
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        REQUIRE( sessions.startSession( configFor( firstDevice.devicePath() ), savePath ) );
        REQUIRE( QFile::remove( savePath ) );

        WHEN( "starting a second port with a different, new save path" )
        {
            const auto started = sessions.startSession( configFor( secondDevice.devicePath() ),
                                                        logDir.filePath( "other.log" ) );

            THEN( "it is accepted: two missing files are not the same file" )
            {
                REQUIRE( started );
                REQUIRE( sessions.isSessionActive( secondDevice.devicePath() ) );
            }
        }

        WHEN( "starting a second port with the same save path, spelled differently" )
        {
            const auto started = sessions.startSession( configFor( secondDevice.devicePath() ),
                                                        logDir.path() + "/sub/../capture.log" );

            THEN( "it is still refused" )
            {
                REQUIRE_FALSE( started );
            }
        }

        sessions.stopAll();
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "a failed rotation is reported once", "[sessions]" )
{
    GIVEN( "a session whose log directory no longer accepts new files" )
    {
        if ( !serial_test::ReadOnlyDir::isEnforced() ) {
            WARN( "File permissions are not enforced (running as root?); skipped." );
            return;
        }

        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        QTemporaryDir logDir;
        REQUIRE( sessions.startSession( configFor( device.devicePath() ),
                                        logDir.filePath( "capture.log" ) ) );
        host.notifications.clear();
        host.openedFiles.clear();

        WHEN( "rotating the session" )
        {
            {
                const serial_test::ReadOnlyDir readOnly( logDir.path() );
                sessions.rotateSession( device.devicePath() );
            }

            THEN( "the user is told once, no tab is opened, and the session goes on" )
            {
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE( sessions.isSessionActive( device.devicePath() ) );
            }
        }

        sessions.stopAll();
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "a capture that reaches its size limit goes on in a new tab", "[sessions]" )
{
    GIVEN( "a session whose log file may hold 16 bytes" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        auto cfg = configFor( device.devicePath() );
        cfg.maxFileBytes = 16;
        cfg.timestamps = false; // keep the file content predictable
        REQUIRE( sessions.startSession( cfg ) );
        const auto firstFile = host.openedFiles.value( 0 );
        host.notifications.clear();

        WHEN( "the device sends more than that" )
        {
            REQUIRE( device.send( "0123456789\nabcdefghij\n" ) );
            const bool rotated = waitFor( [ &host ]() { return host.openedFiles.size() == 2; } );

            THEN( "the capture goes on in a new file, opened in a new tab" )
            {
                REQUIRE( rotated );
                REQUIRE( host.openedFiles.at( 1 ) != firstFile );
                REQUIRE( sessions.isSessionActive( device.devicePath() ) );
            }

            THEN( "the user is told why, and the old file keeps its lines" )
            {
                REQUIRE( rotated );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "size limit" ) );
                QFile old( firstFile );
                REQUIRE( old.open( QIODevice::ReadOnly ) );
                REQUIRE( old.readAll() == "0123456789\nabcdefghij\n" );
            }
        }

        sessions.stopAll();
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "stopAll keeps temporary log files for their tabs", "[sessions]" )
{
    GIVEN( "a session writing to a temporary file" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        auto* sessions = new Sessions( host );
        REQUIRE( sessions->startSession( configFor( device.devicePath() ) ) );
        REQUIRE( host.openedFiles.size() == 1 );
        const auto tempFile = host.openedFiles.first();
        const auto scansBefore = host.logs.filter( "Discovered" ).size();

        WHEN( "the user stops all sessions: stopAll(), then the sessions are deleted" )
        {
            sessions->stopAll();
            const auto scansDuringStop = host.logs.filter( "Discovered" ).size() - scansBefore;
            delete sessions;

            THEN( "the temporary file is kept for its tab" )
            {
                REQUIRE( QFileInfo::exists( tempFile ) );
            }

            THEN( "the ports are not scanned again for the stopped session" )
            {
                REQUIRE( scansDuringStop == 0 );
            }
        }
    }

    GIVEN( "a session writing to a temporary file that has been rotated" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        auto* sessions = new Sessions( host );
        REQUIRE( sessions->startSession( configFor( device.devicePath() ) ) );
        sessions->rotateSession( device.devicePath() );
        REQUIRE( host.openedFiles.size() == 2 );
        REQUIRE( QFileInfo( host.openedFiles.last() ).absolutePath()
                 == QFileInfo( host.openedFiles.first() ).absolutePath() );

        WHEN( "the user stops all sessions: stopAll(), then the sessions are deleted" )
        {
            sessions->stopAll();
            delete sessions;

            THEN( "the files of both tabs are kept" )
            {
                REQUIRE( QFileInfo::exists( host.openedFiles.first() ) );
                REQUIRE( QFileInfo::exists( host.openedFiles.last() ) );
            }
        }
    }
}
#endif

#ifdef Q_OS_UNIX
// A pseudo-terminal cannot be unplugged: QSerialPort does not notice when
// its other side closes.  A real USB adapter that is pulled makes the port
// report QSerialPort::ResourceError, so the test has the session's port
// report it (porterror.h).
SCENARIO( "an unplugged device ends its session", "[sessions]" )
{
    GIVEN( "a running session" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        REQUIRE( sessions.startSession( configFor( device.devicePath() ) ) );
        auto* session = sessions.findChild<serial_monitor::SerialProcess*>();
        REQUIRE( session );
        host.notifications.clear();

        WHEN( "the port reports that the device is gone, twice" )
        {
            for ( int i = 0; i < 2; ++i ) {
                serial_test::reportPortError( session, QSerialPort::ResourceError );
            }

            THEN( "the session is removed" )
            {
                REQUIRE( waitFor( [ &sessions, &device ]() {
                    return !sessions.isSessionActive( device.devicePath() );
                } ) );
            }

            THEN( "the user is told once that the device was disconnected" )
            {
                serial_test::processEventsFor( 100 );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "disconnected" ) );
            }
        }
    }
}
#endif

#ifdef Q_OS_UNIX
// Some drivers report a pulled adapter, or a failure of their own, as a
// ReadError or an UnknownError rather than a ResourceError (Windows
// USB-serial drivers among them); the read notifier then fires on every
// pass of the event loop.
SCENARIO( "any error of an open port ends its session, with one notification", "[sessions]" )
{
    GIVEN( "a running session" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        REQUIRE( sessions.startSession( configFor( device.devicePath() ) ) );
        auto* session = sessions.findChild<serial_monitor::SerialProcess*>();
        REQUIRE( session );
        host.notifications.clear();
        const auto reportError = [ session ]( QSerialPort::SerialPortError error ) {
            serial_test::reportPortError( session, error );
        };

        WHEN( "the port reports a ReadError, again and again, then an UnknownError" )
        {
            for ( int i = 0; i < 5; ++i ) {
                reportError( QSerialPort::ReadError );
            }
            reportError( QSerialPort::UnknownError );

            THEN( "the session is removed, and the user is told once" )
            {
                REQUIRE( waitFor( [ &sessions, &device ]() {
                    return !sessions.isSessionActive( device.devicePath() );
                } ) );
                serial_test::processEventsFor( 100 );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "capture stopped" ) );
            }
        }

        WHEN( "the port reports an UnknownError" )
        {
            reportError( QSerialPort::UnknownError );

            THEN( "the session is removed" )
            {
                REQUIRE( waitFor( [ &sessions, &device ]() {
                    return !sessions.isSessionActive( device.devicePath() );
                } ) );
            }
        }

        WHEN( "the port reports a timeout, which only a wait for data can cause" )
        {
            reportError( QSerialPort::TimeoutError );
            serial_test::processEventsFor( 100 );

            THEN( "the session goes on, and nothing is shown" )
            {
                REQUIRE( sessions.isSessionActive( device.devicePath() ) );
                REQUIRE( host.notifications.isEmpty() );
            }
        }

        sessions.stopAll();
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "a log file that cannot be written ends its session", "[sessions]" )
{
    GIVEN( "a running session whose log file may grow by a few bytes only" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        QTemporaryDir logDir;
        REQUIRE( sessions.startSession( configFor( device.devicePath() ),
                                        logDir.filePath( "capture.log" ) ) );
        host.notifications.clear();
        serial_test::FileSizeLimit limit( 8 );
        REQUIRE( limit.isActive() );

        WHEN( "the device sends more than that" )
        {
            REQUIRE( device.send( "first line\nsecond line\n" ) );
            const bool removed = waitFor( [ &sessions, &device ]() {
                return !sessions.isSessionActive( device.devicePath() );
            } );
            serial_test::processEventsFor( 100 );
            limit.lift();

            THEN( "the session is removed, and the user is told once why" )
            {
                REQUIRE( removed );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "Could not write the log file" ) );
            }
        }
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "starting and stopping a session does not rescan the ports", "[sessions]" )
{
    GIVEN( "sessions that have scanned the ports once" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        const auto scansBefore = host.logs.filter( "Discovered" ).size();
        REQUIRE( scansBefore == 1 );

        WHEN( "a session is started and stopped" )
        {
            REQUIRE( sessions.startSession( configFor( device.devicePath() ) ) );
            sessions.stopSession( device.devicePath() );

            THEN( "the ports are not enumerated again" )
            {
                REQUIRE( host.logs.filter( "Discovered" ).size() == scansBefore );
            }

            QDir( QFileInfo( host.openedFiles.first() ).absolutePath() ).removeRecursively();
        }
    }
}
#endif

SCENARIO( "the port list is rescanned on request", "[sessions]" )
{
    GIVEN( "the sessions" )
    {
        FakeHost host;
        Sessions sessions( host );
        int changes = 0;
        QObject::connect( &sessions, &Sessions::portsChanged, [ &changes ]() { ++changes; } );
        const auto scansBefore = host.logs.filter( "Discovered" ).size();

        WHEN( "refreshing the ports" )
        {
            sessions.refreshPorts();

            THEN( "they are enumerated once and the change announced" )
            {
                REQUIRE( host.logs.filter( "Discovered" ).size() == scansBefore + 1 );
                REQUIRE( changes == 1 );
            }
        }
    }
}

namespace {

/**
 * The signals @p sessions emits about its sessions, in order, as
 * "started <port>", "stopped <port>" or "changed <port>", each followed by
 * " (active)" if the session counted as active when the signal came.
 */
class SessionEvents {
public:
    explicit SessionEvents( const Sessions& sessions )
    {
        const auto record = [ this, &sessions ]( const char* what ) {
            return [ this, &sessions, what ]( const QString& port ) {
                events << QString( "%1 %2%3" )
                              .arg( what, port,
                                    sessions.isSessionActive( port ) ? " (active)" : "" );
            };
        };
        QObject::connect( &sessions, &Sessions::sessionStarted, record( "started" ) );
        QObject::connect( &sessions, &Sessions::sessionStopped, record( "stopped" ) );
        QObject::connect( &sessions, &Sessions::sessionChanged, record( "changed" ) );
    }

    QStringList events;
};

} // namespace

SCENARIO( "the sessions announce what happens to them", "[sessions][signals]" )
{
    GIVEN( "the sessions and a port that does not exist" )
    {
        FakeHost host;
        Sessions sessions( host );
        SessionEvents recorded( sessions );

        WHEN( "a session cannot start" )
        {
            REQUIRE_FALSE( sessions.startSession( configFor( "logsquirl-test-no-such-port" ) ) );

            THEN( "nothing is announced" )
            {
                REQUIRE( recorded.events.isEmpty() );
            }
        }
    }

#ifdef Q_OS_UNIX
    GIVEN( "the sessions and two devices" )
    {
        FakeHost host;
        serial_test::PseudoTerminal first;
        serial_test::PseudoTerminal second;
        Sessions sessions( host );
        SessionEvents recorded( sessions );
        const auto firstPort = first.devicePath();
        const auto secondPort = second.devicePath();

        WHEN( "a session starts" )
        {
            REQUIRE( sessions.startSession( configFor( firstPort ) ) );

            THEN( "its start is announced once it runs and its tab is open" )
            {
                REQUIRE( recorded.events == QStringList{ "started " + firstPort + " (active)" } );
                REQUIRE( host.openedFiles.size() == 1 );
            }

            AND_WHEN( "the user stops it" )
            {
                recorded.events.clear();
                sessions.stopSession( firstPort );

                THEN( "its end is announced once it no longer counts as active" )
                {
                    REQUIRE( recorded.events == QStringList{ "stopped " + firstPort } );
                }
            }

            AND_WHEN( "its log is rotated" )
            {
                recorded.events.clear();
                sessions.rotateSession( firstPort );

                THEN( "the change is announced" )
                {
                    REQUIRE( recorded.events
                             == QStringList{ "changed " + firstPort + " (active)" } );
                }
            }

            AND_WHEN( "its device is unplugged" )
            {
                recorded.events.clear();
                serial_test::reportPortError( sessions.findChild<serial_monitor::SerialProcess*>(),
                                              QSerialPort::ResourceError );

                THEN( "its end is announced, once" )
                {
                    REQUIRE( waitFor( [ &recorded ]() { return !recorded.events.isEmpty(); } ) );
                    serial_test::processEventsFor( 100 );
                    REQUIRE( recorded.events == QStringList{ "stopped " + firstPort } );
                }
            }

            sessions.stopAll();
        }

        WHEN( "two sessions run and all are stopped" )
        {
            REQUIRE( sessions.startSession( configFor( firstPort ) ) );
            REQUIRE( sessions.startSession( configFor( secondPort ) ) );
            recorded.events.clear();
            sessions.stopAll();

            THEN( "the end of each is announced" )
            {
                auto events = recorded.events;
                events.sort();
                auto expected = QStringList{ "stopped " + firstPort, "stopped " + secondPort };
                expected.sort();
                REQUIRE( events == expected );
            }
        }
    }

    GIVEN( "a session whose log file may hold 16 bytes" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        auto cfg = configFor( device.devicePath() );
        cfg.maxFileBytes = 16;
        REQUIRE( sessions.startSession( cfg ) );
        SessionEvents recorded( sessions );

        WHEN( "the device sends more than that" )
        {
            REQUIRE( device.send( "0123456789\nabcdefghij\n" ) );

            THEN( "the rotation to a new file is announced" )
            {
                REQUIRE( waitFor( [ &recorded ]() { return !recorded.events.isEmpty(); } ) );
                REQUIRE( recorded.events
                         == QStringList{ "changed " + device.devicePath() + " (active)" } );
            }
        }

        sessions.stopAll();
    }
#endif
}

#ifdef Q_OS_UNIX
SCENARIO( "a running session's counters can be read through the sessions", "[sessions][counters]" )
{
    GIVEN( "a running session" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        auto cfg = configFor( device.devicePath() );
        cfg.timestamps = false;
        REQUIRE( sessions.startSession( cfg ) );
        const auto* session = sessions.session( device.devicePath() );
        REQUIRE( session );

        WHEN( "the device sends lines and the log is rotated" )
        {
            REQUIRE( device.send( "one\ntwo\n" ) );
            REQUIRE( waitFor( [ session ]() { return session->totalLineCount() == 2; } ) );
            sessions.rotateSession( device.devicePath() );

            THEN( "the session's counters say so" )
            {
                REQUIRE( session->receivedBytes() == 8 );
                REQUIRE( session->rotationCount() == 1 );
                REQUIRE( session->fileSize() == 0 );
                REQUIRE( session->startTime().isValid() );
            }
        }

        WHEN( "the session is stopped" )
        {
            sessions.stopSession( device.devicePath() );

            THEN( "there is no session to read any more" )
            {
                REQUIRE( sessions.session( device.devicePath() ) == nullptr );
            }
        }

        sessions.stopAll();
    }

    GIVEN( "a port without a session" )
    {
        FakeHost host;
        Sessions sessions( host );

        THEN( "there is no session to read" )
        {
            REQUIRE( sessions.session( "logsquirl-test-no-such-port" ) == nullptr );
        }
    }
}

SCENARIO( "a session remembers the configuration that started it", "[sessions][byname]" )
{
    GIVEN( "a session started by the configuration \"board\"" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        REQUIRE( sessions.startSession( configFor( device.devicePath() ), {}, "board" ) );

        THEN( "the configuration runs, its capture is the port's, and its port says by whom" )
        {
            REQUIRE( sessions.isRunning( "board" ) );
            REQUIRE_FALSE( sessions.isRunning( "other" ) );
            REQUIRE_FALSE( sessions.isRunning( {} ) );
            REQUIRE( sessions.captureOf( "board" ) == sessions.session( device.devicePath() ) );
            REQUIRE( sessions.captureOf( "other" ) == nullptr );
            REQUIRE( sessions.portInUse( device.devicePath() )
                     == device.devicePath() + " is in use by “board”" );
        }

        THEN( "the store's check asks the same" )
        {
            const auto isRunning = sessions.runningCheck();
            REQUIRE( isRunning( "board" ) );
            REQUIRE_FALSE( isRunning( "other" ) );
        }

        WHEN( "it is stopped" )
        {
            sessions.stopSession( device.devicePath() );

            THEN( "it is not remembered any more" )
            {
                REQUIRE_FALSE( sessions.isRunning( "board" ) );
                REQUIRE( sessions.captureOf( "board" ) == nullptr );
                REQUIRE( sessions.portInUse( device.devicePath() ).isEmpty() );
            }
        }

        WHEN( "it ends on its own, its device unplugged" )
        {
            // As in "an unplugged device ends its session": a pseudo-terminal
            // cannot be unplugged, so the port reports the error itself.
            serial_test::reportPortError( sessions.findChild<serial_monitor::SerialProcess*>(),
                                          QSerialPort::ResourceError );

            THEN( "it is not remembered any more" )
            {
                REQUIRE( waitFor( [ &sessions ]() { return !sessions.isRunning( "board" ); } ) );
                REQUIRE( sessions.captureOf( "board" ) == nullptr );
                REQUIRE( sessions.portInUse( device.devicePath() ).isEmpty() );
            }
        }

        sessions.stopAll();
    }

    GIVEN( "a session started without a configuration" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        REQUIRE( sessions.startSession( configFor( device.devicePath() ) ) );

        THEN( "no configuration runs, and its port is in use by another capture" )
        {
            REQUIRE_FALSE( sessions.isRunning( {} ) );
            REQUIRE( sessions.captureOf( {} ) == nullptr );
            REQUIRE( sessions.portInUse( device.devicePath() )
                     == device.devicePath() + " is in use by another capture" );
        }

        sessions.stopAll();
    }
}

SCENARIO( "a configuration's capture is stopped, rotated and sent to by its name",
          "[sessions][byname]" )
{
    GIVEN( "the configuration \"board\" capturing a device, and \"console\" another" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        serial_test::PseudoTerminal otherDevice;
        Sessions sessions( host );
        QTemporaryDir logDir;
        auto config = configFor( device.devicePath() );
        config.timestamps = false;
        REQUIRE( sessions.startSession( config, logDir.filePath( "board.log" ), "board" ) );
        REQUIRE( sessions.startSession( configFor( otherDevice.devicePath() ),
                                        logDir.filePath( "console.log" ), "console" ) );
        host.notifications.clear();
        host.followedFiles.clear();

        WHEN( "\"board\" is sent a command" )
        {
            const auto sent = sessions.send( "board", "status", serial_monitor::TxLineEnding::LF );

            THEN( "its device receives it, and its log has it" )
            {
                REQUIRE( sent );
                QByteArray received;
                REQUIRE( waitFor( [ &device, &received ]() {
                    received += device.receive();
                    return received == "status\n";
                } ) );
                REQUIRE( waitFor( [ &sessions ]() {
                    return sessions.captureOf( "board" )->totalLineCount() == 1;
                } ) );
            }
        }

        WHEN( "\"board\" is rotated" )
        {
            sessions.rotate( "board" );

            THEN( "its capture goes on in a new file, the other's does not" )
            {
                REQUIRE( host.followedFiles.size() == 1 );
                REQUIRE( sessions.captureOf( "board" )->rotationCount() == 1 );
                REQUIRE( sessions.captureOf( "console" )->rotationCount() == 0 );
            }
        }

        WHEN( "\"board\" is stopped" )
        {
            sessions.stop( "board" );

            THEN( "its capture ends, the other's goes on" )
            {
                REQUIRE_FALSE( sessions.isRunning( "board" ) );
                REQUIRE_FALSE( sessions.isSessionActive( device.devicePath() ) );
                REQUIRE( sessions.isRunning( "console" ) );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().startsWith( "Serial capture stopped for "
                                                                + device.devicePath() ) );
            }
        }

        sessions.stopAll();
    }
}

SCENARIO( "a configuration that does not capture is stopped, rotated and sent to quietly",
          "[sessions][byname]" )
{
    GIVEN( "the configuration \"board\", whose capture has just ended" )
    {
        // Stopped, or ended on its own (its device unplugged).
        const bool unplugged = GENERATE( false, true );
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        QTemporaryDir logDir;
        REQUIRE( sessions.startSession( configFor( device.devicePath() ),
                                        logDir.filePath( "board.log" ), "board" ) );
        int stopped = 0;
        int changed = 0;
        QObject::connect( &sessions, &Sessions::sessionStopped,
                          [ &stopped ]( const QString& ) { ++stopped; } );
        QObject::connect( &sessions, &Sessions::sessionChanged,
                          [ &changed ]( const QString& ) { ++changed; } );

        if ( !unplugged ) {
            sessions.stop( "board" );
        }
        else {
            serial_test::reportPortError( sessions.findChild<serial_monitor::SerialProcess*>(),
                                          QSerialPort::ResourceError );
            REQUIRE( waitFor( [ &sessions ]() { return !sessions.isRunning( "board" ); } ) );
        }

        REQUIRE( stopped == 1 );
        stopped = 0;
        host.logs.clear();
        host.notifications.clear();
        host.openedFiles.clear();

        THEN( "stopping, rotating and sending it do nothing, and tell nothing" )
        {
            sessions.stop( "board" );
            sessions.rotate( "board" );
            REQUIRE_FALSE( sessions.send( "board", "status", serial_monitor::TxLineEnding::LF ) );

            REQUIRE( host.logs.isEmpty() );
            REQUIRE( host.notifications.isEmpty() );
            REQUIRE( host.openedFiles.isEmpty() );
            REQUIRE( stopped == 0 );
            REQUIRE( changed == 0 );
            REQUIRE( sessions.captureOf( "board" ) == nullptr );
        }

        sessions.stopAll();
    }

    GIVEN( "a port captured by \"board\", and the configuration \"console\" on it, stopped" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        REQUIRE( sessions.startSession( configFor( device.devicePath() ), {}, "board" ) );
        host.logs.clear();
        host.notifications.clear();
        host.openedFiles.clear();

        WHEN( "\"console\" is stopped, rotated and sent to" )
        {
            sessions.stop( "console" );
            sessions.rotate( "console" );
            const auto sent
                = sessions.send( "console", "status", serial_monitor::TxLineEnding::LF );

            THEN( "the capture of \"board\" goes on untouched, and nothing is told" )
            {
                REQUIRE_FALSE( sent );
                REQUIRE( sessions.isRunning( "board" ) );
                REQUIRE( sessions.captureOf( "board" )->rotationCount() == 0 );
                REQUIRE( host.logs.isEmpty() );
                REQUIRE( host.notifications.isEmpty() );
                REQUIRE( host.openedFiles.isEmpty() );
            }
        }

        sessions.stopAll();
    }

    GIVEN( "no capture at all" )
    {
        FakeHost host;
        Sessions sessions( host );
        host.logs.clear();

        THEN( "stopping, rotating and sending a configuration, or none, tell nothing" )
        {
            for ( const QString& name : { QString( "board" ), QString() } ) {
                sessions.stop( name );
                sessions.rotate( name );
                REQUIRE_FALSE( sessions.send( name, "status", serial_monitor::TxLineEnding::LF ) );
                REQUIRE( sessions.captureOf( name ) == nullptr );
                REQUIRE_FALSE( sessions.isRunning( name ) );
            }
            REQUIRE( host.logs.isEmpty() );
            REQUIRE( host.notifications.isEmpty() );
            REQUIRE( sessions.portInUse( "logsquirl-test-no-such-port" ).isEmpty() );
        }
    }
}

SCENARIO( "a port that already captures is refused with a message", "[sessions]" )
{
    GIVEN( "a port captured for the configuration \"board\"" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        REQUIRE( sessions.startSession( configFor( device.devicePath() ), {}, "board" ) );
        host.notifications.clear();
        host.openedFiles.clear();

        WHEN( "another configuration starts a capture of the same port" )
        {
            const auto started
                = sessions.startSession( configFor( device.devicePath() ), {}, "console" );

            THEN( "it is refused, and the user is told which capture holds the port" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( device.devicePath() ) );
                REQUIRE( host.notifications.first().contains( "board" ) );
                REQUIRE( sessions.isRunning( "board" ) );
                REQUIRE_FALSE( sessions.isRunning( "console" ) );
            }
        }

        sessions.stopAll();
    }

    GIVEN( "a port captured without a configuration" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        REQUIRE( sessions.startSession( configFor( device.devicePath() ) ) );
        host.notifications.clear();

        WHEN( "it is started again" )
        {
            REQUIRE_FALSE( sessions.startSession( configFor( device.devicePath() ) ) );

            THEN( "the user is told that the port is already being captured" )
            {
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "already" ) );
            }
        }

        sessions.stopAll();
    }
}

// What a session reports reaches Sessions in LogSquirl's event loop: an
// exception there must be logged, not escape into the host (#53).  The
// FakeHost's log throws on the message a handler writes, or a slot on the
// signal a handler emits throws, as a failing allocation or container could.
SCENARIO( "an exception in what a session reports does not escape", "[sessions][guarded]" )
{
    GIVEN( "a running session" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions( host );
        REQUIRE( sessions.startSession( configFor( device.devicePath() ) ) );
        auto* session = sessions.findChild<serial_monitor::SerialProcess*>();
        REQUIRE( session );
        const auto failures = [ &host ] {
            return host.logs.filter( "Serial Monitor plugin" ).filter( "failed: " );
        };

        WHEN( "logging its end throws" )
        {
            host.failLogContaining = "session for";
            REQUIRE_NOTHROW( session->stop() );

            THEN( "the failure is logged" )
            {
                REQUIRE( failures().size() == 1 );
                REQUIRE( failures().first().contains( "the log is full" ) );
            }
        }

        WHEN( "logging its error throws" )
        {
            host.failLogContaining = "the cable is out";
            REQUIRE_NOTHROW( Q_EMIT session->errorOccurred( "the cable is out" ) );

            THEN( "the failure is logged" )
            {
                REQUIRE( failures().size() == 1 );
                REQUIRE( failures().first().contains( "the log is full" ) );
            }
        }

        WHEN( "announcing the new file at its size limit throws" )
        {
            QObject::connect( &sessions, &Sessions::sessionChanged,
                              []( const QString& ) { throw std::runtime_error( "no memory" ); } );
            REQUIRE_NOTHROW( Q_EMIT session->sizeLimitReached() );

            THEN( "the failure is logged" )
            {
                REQUIRE( failures().size() == 1 );
                REQUIRE( failures().first().contains( "no memory" ) );
            }
        }

        host.failLogContaining.clear();
        sessions.stopAll();
    }
}
#endif

// Every refusal is a Host::warn(): one log entry, as a warning, and one
// notification, with the same text.  An error a session reports is one log
// entry, as an error, and one notification.
SCENARIO( "the sessions tell the host they are handed, once each", "[sessions][host]" )
{
    GIVEN( "sessions on a fake host, after the port scan" )
    {
        FakeHost host;
        Sessions sessions( host );

        THEN( "the scan is logged, and nothing is notified" )
        {
            REQUIRE( host.logs.filter( "Discovered" ).size() == 1 );
            REQUIRE( host.notifications.isEmpty() );
        }

        host.logs.clear();
        host.logLevels.clear();

        WHEN( "a session with an invalid baud rate is refused" )
        {
            auto config = configFor( "logsquirl-test-no-such-port" );
            config.baudRate = 0;
            REQUIRE_FALSE( sessions.startSession( config ) );

            THEN( "the host logs one warning and shows one notification, saying the same" )
            {
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "invalid baud rate" ) );
                REQUIRE( host.logs == host.notifications );
                REQUIRE( host.logLevels == QList<int>{ LOGSQUIRL_LOG_WARNING } );
            }
        }

        WHEN( "a session whose port does not open is refused" )
        {
            REQUIRE_FALSE( sessions.startSession( configFor( "logsquirl-test-no-such-port" ) ) );

            THEN( "the host logs one error and shows one notification, both naming the port" )
            {
                REQUIRE( host.logs.size() == 1 );
                REQUIRE( host.logLevels == QList<int>{ LOGSQUIRL_LOG_ERROR } );
                REQUIRE( host.logs.first().startsWith( "logsquirl-test-no-such-port: " ) );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().startsWith(
                    "Serial error (logsquirl-test-no-such-port): " ) );
                REQUIRE( host.openedFiles.isEmpty() );
            }
        }
    }

#ifdef Q_OS_UNIX
    GIVEN( "a running session on a fake host" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        serial_test::PseudoTerminal otherDevice;
        Sessions sessions( host );
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        REQUIRE( sessions.startSession( configFor( device.devicePath() ), savePath, "board" ) );

        THEN( "its start opens one followed tab and is one notification" )
        {
            REQUIRE( host.followedFiles == QStringList{ savePath } );
            REQUIRE( host.notifications.size() == 1 );
            REQUIRE( host.notifications.first().startsWith( "Serial capture started for" ) );
        }

        host.logs.clear();
        host.logLevels.clear();
        host.notifications.clear();

        WHEN( "its port is started again" )
        {
            REQUIRE_FALSE( sessions.startSession( configFor( device.devicePath() ) ) );

            THEN( "the host logs one warning and shows one notification naming the owner" )
            {
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "“board”" ) );
                REQUIRE( host.logs == host.notifications );
                REQUIRE( host.logLevels == QList<int>{ LOGSQUIRL_LOG_WARNING } );
            }
        }

        WHEN( "another port is started on its save path" )
        {
            REQUIRE_FALSE(
                sessions.startSession( configFor( otherDevice.devicePath() ), savePath ) );

            THEN( "the host logs one warning and shows one notification, saying the same" )
            {
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "already writing to" ) );
                REQUIRE( host.logs == host.notifications );
                REQUIRE( host.logLevels == QList<int>{ LOGSQUIRL_LOG_WARNING } );
            }
        }

        WHEN( "it is stopped" )
        {
            sessions.stopSession( device.devicePath() );

            THEN( "the host shows one notification, and logs the port closed" )
            {
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().startsWith( "Serial capture stopped for" ) );
                REQUIRE( host.logs.filter( "Closed " + device.devicePath() ).size() == 1 );
            }
        }

        sessions.stopAll();
    }
#endif
}
