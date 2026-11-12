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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;
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
        auto* sessions = new Sessions;
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
        auto* sessions = new Sessions;
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
// report QSerialPort::ResourceError, so the test delivers that error to
// the session's handler directly.
SCENARIO( "an unplugged device ends its session", "[sessions]" )
{
    GIVEN( "a running session" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions;
        REQUIRE( sessions.startSession( configFor( device.devicePath() ) ) );
        auto* session = sessions.findChild<serial_monitor::SerialProcess*>();
        REQUIRE( session );
        host.notifications.clear();

        WHEN( "the port reports that the device is gone, twice" )
        {
            for ( int i = 0; i < 2; ++i ) {
                QMetaObject::invokeMethod(
                    session, "onPortError",
                    Q_ARG( QSerialPort::SerialPortError, QSerialPort::ResourceError ) );
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
        Sessions sessions;
        REQUIRE( sessions.startSession( configFor( device.devicePath() ) ) );
        auto* session = sessions.findChild<serial_monitor::SerialProcess*>();
        REQUIRE( session );
        host.notifications.clear();
        const auto reportError = [ session ]( QSerialPort::SerialPortError error ) {
            QMetaObject::invokeMethod( session, "onPortError",
                                       Q_ARG( QSerialPort::SerialPortError, error ) );
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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;
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
                auto* session = sessions.findChild<serial_monitor::SerialProcess*>();
                REQUIRE( session );
                QMetaObject::invokeMethod(
                    session, "onPortError",
                    Q_ARG( QSerialPort::SerialPortError, QSerialPort::ResourceError ) );

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
        Sessions sessions;
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
        Sessions sessions;
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
        Sessions sessions;

        THEN( "there is no session to read" )
        {
            REQUIRE( sessions.session( "logsquirl-test-no-such-port" ) == nullptr );
        }
    }
}
#endif
