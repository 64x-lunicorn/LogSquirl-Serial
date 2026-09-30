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
 * @file portwidget_test.cpp
 * @brief BDD tests for the session bookkeeping in PortWidget.
 *
 * PortWidget owns the running SerialProcess sessions for both the dialog
 * and the sidebar.  These tests drive it through its public API with a
 * FakeHost and, on Unix, a pseudo-terminal as the device (see
 * pseudoterminal.h).
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "portwidget.h"
#include "pseudoterminal.h"
#include "readonlydir.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using serial_monitor::PortWidget;
using serial_monitor::SerialConfig;
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

SCENARIO( "a session is only accepted when its port opens", "[portwidget]" )
{
    GIVEN( "a port that does not exist" )
    {
        FakeHost host;
        PortWidget widget;
        QTemporaryDir logDir;
        const QString port = "logsquirl-test-no-such-port";

        WHEN( "starting a session with a save path" )
        {
            const auto started
                = widget.startSession( configFor( port ), logDir.filePath( "capture.log" ) );

            THEN( "the session is rejected" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( widget.activeSessionCount() == 0 );
                REQUIRE_FALSE( widget.isSessionActive( port ) );
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

#ifdef Q_OS_UNIX
    GIVEN( "a device on a working port" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        PortWidget widget;
        QTemporaryDir logDir;

        WHEN( "starting a session" )
        {
            const auto started = widget.startSession( configFor( device.devicePath() ),
                                                      logDir.filePath( "capture.log" ) );

            THEN( "the session is accepted and its file opened in a tab" )
            {
                REQUIRE( started );
                REQUIRE( widget.isSessionActive( device.devicePath() ) );
                REQUIRE( host.openedFiles.size() == 1 );
            }

            widget.stopAll();
        }
    }
#endif
}

#ifdef Q_OS_UNIX
SCENARIO( "two sessions never write to the same file", "[portwidget]" )
{
    GIVEN( "a session writing to a save path" )
    {
        FakeHost host;
        serial_test::PseudoTerminal firstDevice;
        serial_test::PseudoTerminal secondDevice;
        PortWidget widget;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        REQUIRE( widget.startSession( configFor( firstDevice.devicePath() ), savePath ) );
        host.notifications.clear();

        WHEN( "starting a second port with the same save path" )
        {
            const auto started
                = widget.startSession( configFor( secondDevice.devicePath() ), savePath );

            THEN( "the second session is refused, and the user told why" )
            {
                REQUIRE_FALSE( started );
                REQUIRE_FALSE( widget.isSessionActive( secondDevice.devicePath() ) );
                REQUIRE( host.notifications.size() == 1 );
            }
        }

        widget.stopAll();
    }

    GIVEN( "a session whose save file has been deleted while it runs" )
    {
        FakeHost host;
        serial_test::PseudoTerminal firstDevice;
        serial_test::PseudoTerminal secondDevice;
        PortWidget widget;
        QTemporaryDir logDir;
        const auto savePath = logDir.filePath( "capture.log" );
        REQUIRE( widget.startSession( configFor( firstDevice.devicePath() ), savePath ) );
        REQUIRE( QFile::remove( savePath ) );

        WHEN( "starting a second port with a different, new save path" )
        {
            const auto started = widget.startSession( configFor( secondDevice.devicePath() ),
                                                      logDir.filePath( "other.log" ) );

            THEN( "it is accepted: two missing files are not the same file" )
            {
                REQUIRE( started );
                REQUIRE( widget.isSessionActive( secondDevice.devicePath() ) );
            }
        }

        WHEN( "starting a second port with the same save path, spelled differently" )
        {
            const auto started = widget.startSession( configFor( secondDevice.devicePath() ),
                                                      logDir.path() + "/sub/../capture.log" );

            THEN( "it is still refused" )
            {
                REQUIRE_FALSE( started );
            }
        }

        widget.stopAll();
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "a failed rotation is reported once", "[portwidget]" )
{
    GIVEN( "a session whose log directory no longer accepts new files" )
    {
        if ( !serial_test::ReadOnlyDir::isEnforced() ) {
            WARN( "File permissions are not enforced (running as root?); skipped." );
            return;
        }

        FakeHost host;
        serial_test::PseudoTerminal device;
        PortWidget widget;
        QTemporaryDir logDir;
        REQUIRE( widget.startSession( configFor( device.devicePath() ),
                                      logDir.filePath( "capture.log" ) ) );
        host.notifications.clear();
        host.openedFiles.clear();

        WHEN( "rotating the session" )
        {
            {
                const serial_test::ReadOnlyDir readOnly( logDir.path() );
                widget.rotateSession( device.devicePath() );
            }

            THEN( "the user is told once, no tab is opened, and the session goes on" )
            {
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE( widget.isSessionActive( device.devicePath() ) );
            }
        }

        widget.stopAll();
    }
}
#endif

#ifdef Q_OS_UNIX
SCENARIO( "stopAll decides whether temporary log files survive", "[portwidget]" )
{
    GIVEN( "a session writing to a temporary file" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        auto* widget = new PortWidget;
        REQUIRE( widget->startSession( configFor( device.devicePath() ) ) );
        REQUIRE( host.openedFiles.size() == 1 );
        const auto tempFile = host.openedFiles.first();
        const auto scansBefore = host.logs.filter( "Discovered" ).size();

        WHEN( "the plugin shuts down: stopAll( true ), then the widget is deleted" )
        {
            widget->stopAll( true );
            const auto scansDuringStop = host.logs.filter( "Discovered" ).size() - scansBefore;
            delete widget;

            THEN( "the temporary file is removed" )
            {
                REQUIRE_FALSE( QFileInfo::exists( tempFile ) );
            }

            THEN( "the ports are not scanned again for the stopped session" )
            {
                REQUIRE( scansDuringStop == 0 );
            }
        }

        WHEN( "the user stops all sessions: stopAll(), then the widget is deleted" )
        {
            widget->stopAll();
            delete widget;

            THEN( "the temporary file is kept for its tab" )
            {
                REQUIRE( QFileInfo::exists( tempFile ) );
            }

            QDir( QFileInfo( tempFile ).absolutePath() ).removeRecursively();
        }
    }

    GIVEN( "a session writing to a temporary file that has been rotated" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        auto* widget = new PortWidget;
        REQUIRE( widget->startSession( configFor( device.devicePath() ) ) );
        widget->rotateSession( device.devicePath() );
        REQUIRE( host.openedFiles.size() == 2 );
        const auto tempDir = QFileInfo( host.openedFiles.first() ).absolutePath();
        REQUIRE( QFileInfo( host.openedFiles.last() ).absolutePath() == tempDir );

        WHEN( "the plugin shuts down: stopAll( true ), then the widget is deleted" )
        {
            widget->stopAll( true );
            delete widget;

            THEN( "the temporary directory is removed with the files of both tabs" )
            {
                REQUIRE_FALSE( QFileInfo::exists( tempDir ) );
            }
        }

        WHEN( "the user stops all sessions: stopAll(), then the widget is deleted" )
        {
            widget->stopAll();
            delete widget;

            THEN( "the files of both tabs are kept" )
            {
                REQUIRE( QFileInfo::exists( host.openedFiles.first() ) );
                REQUIRE( QFileInfo::exists( host.openedFiles.last() ) );
            }

            QDir( tempDir ).removeRecursively();
        }
    }
}
#endif

#ifdef Q_OS_UNIX
// A pseudo-terminal cannot be unplugged: QSerialPort does not notice when
// its other side closes.  A real USB adapter that is pulled makes the port
// report QSerialPort::ResourceError, so the test delivers that error to
// the session's handler directly.
SCENARIO( "an unplugged device ends its session", "[portwidget]" )
{
    GIVEN( "a running session" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        PortWidget widget;
        REQUIRE( widget.startSession( configFor( device.devicePath() ) ) );
        auto* session = widget.findChild<serial_monitor::SerialProcess*>();
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
                REQUIRE( waitFor( [ &widget, &device ]() {
                    return !widget.isSessionActive( device.devicePath() );
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
SCENARIO( "starting and stopping a session does not rescan the ports", "[portwidget]" )
{
    GIVEN( "a port widget that has scanned the ports once" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        PortWidget widget;
        const auto scansBefore = host.logs.filter( "Discovered" ).size();
        REQUIRE( scansBefore == 1 );

        WHEN( "a session is started and stopped" )
        {
            REQUIRE( widget.startSession( configFor( device.devicePath() ) ) );
            widget.stopSession( device.devicePath() );

            THEN( "the ports are not enumerated again" )
            {
                REQUIRE( host.logs.filter( "Discovered" ).size() == scansBefore );
            }

            QDir( QFileInfo( host.openedFiles.first() ).absolutePath() ).removeRecursively();
        }
    }
}
#endif

SCENARIO( "the port list is rescanned on request", "[portwidget]" )
{
    GIVEN( "a port widget" )
    {
        FakeHost host;
        PortWidget widget;
        int changes = 0;
        QObject::connect( &widget, &PortWidget::portsChanged, [ &changes ]() { ++changes; } );
        const auto scansBefore = host.logs.filter( "Discovered" ).size();

        WHEN( "refreshing the ports" )
        {
            widget.refreshPorts();

            THEN( "they are enumerated once and the change announced" )
            {
                REQUIRE( host.logs.filter( "Discovered" ).size() == scansBefore + 1 );
                REQUIRE( changes == 1 );
            }
        }
    }
}
