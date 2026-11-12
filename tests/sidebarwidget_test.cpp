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
 * @file sidebarwidget_test.cpp
 * @brief BDD tests for the Serial sidebar tab and its tiles.
 *
 * The tab is driven through its tiles' named buttons against a FakeHost;
 * on Unix a pseudo-terminal plays the device of a configuration (see
 * pseudoterminal.h).
 */

#include <catch2/catch.hpp>

#include "connectiontile.h"
#include "fakehost.h"
#include "modal.h"
#include "profiles.h"
#include "pseudoterminal.h"
#include "sessions.h"
#include "sidebarwidget.h"

#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

using serial_monitor::ConnectionTile;
using serial_monitor::PortProfile;
using serial_monitor::ProfileStore;
using serial_monitor::Sessions;
using serial_monitor::SidebarWidget;
using serial_test::FakeHost;
using serial_test::waitFor;

namespace {

/// A store on the FakeHost's settings file that knows which configurations
/// capture, as the plugin's.
struct Store {
    Store( const FakeHost& host, Sessions& sessions )
        : store( host.configDir() + "/serial.ini", [ &sessions ]( const QString& name ) {
            return !sessions.portOf( name ).isEmpty();
        } )
    {
    }
    ProfileStore store;
};

PortProfile profileFor( const QString& name, const QString& port, const QString& logDir = {} )
{
    PortProfile profile;
    profile.name = name;
    profile.serial.portName = port;
    profile.serial.timestamps = false;
    profile.logDir = logDir;
    return profile;
}

QString textOf( const ConnectionTile* tile, const char* label )
{
    REQUIRE( tile );
    return tile->findChild<QLabel*>( label )->text();
}

QString stateOf( const ConnectionTile* tile )
{
    REQUIRE( tile );
    return tile->findChild<QLabel*>( "state" )->property( "state" ).toString();
}

void click( const ConnectionTile* tile, const char* button )
{
    REQUIRE( tile );
    tile->findChild<QPushButton*>( button )->click();
}

} // namespace

SCENARIO( "the tab shows one tile per saved configuration", "[sidebarwidget]" )
{
    GIVEN( "a sidebar without saved configurations" )
    {
        FakeHost host;
        Sessions sessions;
        Store profiles( host, sessions );
        SidebarWidget sidebar( &profiles.store, &sessions );
        sidebar.show();

        THEN( "it says how to add one" )
        {
            REQUIRE( sidebar.findChild<QLabel*>( "empty" )->isVisible() );
            REQUIRE( sidebar.findChildren<ConnectionTile*>().isEmpty() );
        }

        WHEN( "a configuration is saved" )
        {
            REQUIRE( profiles.store.upsert( profileFor( "board", "/dev/ttyUSB0" ) ).isEmpty() );

            THEN( "it gets a tile with its name, port, frame and flow control" )
            {
                const auto* tile = sidebar.tile( "board" );
                REQUIRE( tile );
                REQUIRE( textOf( tile, "title" ) == "board" );
                REQUIRE( textOf( tile, "summary" )
                         == "/dev/ttyUSB0\n115200 8N1 · no flow control" );
                REQUIRE( stateOf( tile ) == "stopped" );
                REQUIRE_FALSE( sidebar.findChild<QLabel*>( "empty" )->isVisible() );
            }

            AND_WHEN( "it is saved again with other settings" )
            {
                auto changed = profileFor( "board", "/dev/ttyACM0" );
                changed.serial.baudRate = 9600;
                changed.serial.flowControl = QSerialPort::HardwareControl;
                REQUIRE( profiles.store.upsert( changed ).isEmpty() );

                THEN( "its tile shows them" )
                {
                    REQUIRE( textOf( sidebar.tile( "board" ), "summary" )
                             == "/dev/ttyACM0\n9600 8N1 · RTS/CTS" );
                }
            }

            AND_WHEN( "a second one is saved" )
            {
                REQUIRE(
                    profiles.store.upsert( profileFor( "console", "/dev/ttyUSB1" ) ).isEmpty() );

                THEN( "both have a tile" )
                {
                    REQUIRE( sidebar.tile( "board" ) );
                    REQUIRE( sidebar.tile( "console" ) );
                }
            }

            AND_WHEN( "it is deleted" )
            {
                REQUIRE( profiles.store.remove( "board" ).isEmpty() );

                THEN( "its tile goes, and the hint is back" )
                {
                    REQUIRE_FALSE( sidebar.tile( "board" ) );
                    REQUIRE( sidebar.findChild<QLabel*>( "empty" )->isVisible() );
                }
            }
        }
    }

    GIVEN( "configurations saved before the sidebar was created, as after a restart" )
    {
        FakeHost host;
        Sessions sessions;
        Store profiles( host, sessions );
        REQUIRE( profiles.store.upsert( profileFor( "board", "/dev/ttyUSB0" ) ).isEmpty() );
        REQUIRE( profiles.store.upsert( profileFor( "console", "/dev/ttyUSB1" ) ).isEmpty() );

        WHEN( "the sidebar is created" )
        {
            SidebarWidget sidebar( &profiles.store, &sessions );

            THEN( "it shows a tile for each, in the order they were saved" )
            {
                const auto tiles = sidebar.findChildren<ConnectionTile*>();
                REQUIRE( tiles.size() == 2 );
                REQUIRE( tiles.at( 0 )->name() == "board" );
                REQUIRE( tiles.at( 1 )->name() == "console" );
            }
        }
    }
}

SCENARIO( "a tile's Delete deletes its configuration once confirmed", "[sidebarwidget]" )
{
    GIVEN( "a tile" )
    {
        FakeHost host;
        Sessions sessions;
        Store profiles( host, sessions );
        REQUIRE( profiles.store.upsert( profileFor( "board", "/dev/ttyUSB0" ) ).isEmpty() );
        SidebarWidget sidebar( &profiles.store, &sessions );

        WHEN( "its Delete is clicked and confirmed" )
        {
            serial_test::answerQuestion( QMessageBox::Yes );
            click( sidebar.tile( "board" ), "delete" );

            THEN( "the configuration and its tile are gone" )
            {
                REQUIRE_FALSE( profiles.store.find( "board" ) );
                REQUIRE_FALSE( sidebar.tile( "board" ) );
            }
        }

        WHEN( "its Delete is clicked and cancelled" )
        {
            serial_test::answerQuestion( QMessageBox::Cancel );
            click( sidebar.tile( "board" ), "delete" );

            THEN( "the configuration and its tile stay" )
            {
                REQUIRE( profiles.store.find( "board" ) );
                REQUIRE( sidebar.tile( "board" ) );
            }
        }
    }
}

SCENARIO( "a tile asks the plugin to open the dialog", "[sidebarwidget]" )
{
    GIVEN( "a sidebar with a tile" )
    {
        FakeHost host;
        Sessions sessions;
        Store profiles( host, sessions );
        REQUIRE( profiles.store.upsert( profileFor( "board", "/dev/ttyUSB0" ) ).isEmpty() );
        SidebarWidget sidebar( &profiles.store, &sessions );
        int newRequests = 0;
        QStringList edits;
        QObject::connect( &sidebar, &SidebarWidget::newConfigurationRequested,
                          [ &newRequests ] { ++newRequests; } );
        QObject::connect( &sidebar, &SidebarWidget::editRequested,
                          [ &edits ]( const QString& name ) { edits << name; } );

        WHEN( "New configuration… and the tile's Edit… are clicked" )
        {
            sidebar.findChild<QPushButton*>( "newConfiguration" )->click();
            click( sidebar.tile( "board" ), "edit" );

            THEN( "the plugin is asked for an empty dialog, and for the tile's configuration" )
            {
                REQUIRE( newRequests == 1 );
                REQUIRE( edits == QStringList{ "board" } );
            }
        }
    }
}

SCENARIO( "a configuration that cannot start says why", "[sidebarwidget]" )
{
    GIVEN( "a tile whose configuration has no port" )
    {
        FakeHost host;
        Sessions sessions;
        Store profiles( host, sessions );
        REQUIRE( profiles.store.upsert( profileFor( "board", {} ) ).isEmpty() );
        SidebarWidget sidebar( &profiles.store, &sessions );
        host.notifications.clear();

        WHEN( "its Start is clicked" )
        {
            click( sidebar.tile( "board" ), "startStop" );

            THEN( "nothing starts, and the user is told why" )
            {
                REQUIRE( sessions.activeSessionCount() == 0 );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "port" ) );
            }
        }
    }

    GIVEN( "a tile whose port does not exist" )
    {
        FakeHost host;
        Sessions sessions;
        Store profiles( host, sessions );
        QTemporaryDir logDir;
        REQUIRE( profiles.store
                     .upsert( profileFor( "board", "logsquirl-test-no-such-port", logDir.path() ) )
                     .isEmpty() );
        SidebarWidget sidebar( &profiles.store, &sessions );
        host.notifications.clear();

        WHEN( "its Start is clicked" )
        {
            click( sidebar.tile( "board" ), "startStop" );

            THEN( "nothing starts, no tab opens, and the user is told why" )
            {
                REQUIRE( sessions.activeSessionCount() == 0 );
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( stateOf( sidebar.tile( "board" ) ) == "stopped" );
            }
        }
    }
}

SCENARIO( "the sidebar does not poll the sessions", "[sidebarwidget][signals]" )
{
    GIVEN( "a sidebar with a tile" )
    {
        FakeHost host;
        Sessions sessions;
        Store profiles( host, sessions );
        REQUIRE( profiles.store.upsert( profileFor( "board", "/dev/ttyUSB0" ) ).isEmpty() );
        SidebarWidget sidebar( &profiles.store, &sessions );

        THEN( "it runs no timer: it updates on the sessions' signals" )
        {
            REQUIRE( sidebar.findChildren<QTimer*>().isEmpty() );
        }
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "a tile starts and stops a capture of its port", "[sidebarwidget]" )
{
    GIVEN( "a tile for a device, writing to a log directory" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions;
        Store profiles( host, sessions );
        QTemporaryDir logDir;
        REQUIRE( profiles.store.upsert( profileFor( "board", device.devicePath(), logDir.path() ) )
                     .isEmpty() );
        SidebarWidget sidebar( &profiles.store, &sessions );
        sidebar.show();

        WHEN( "its Start is clicked" )
        {
            click( sidebar.tile( "board" ), "startStop" );

            THEN( "the port captures, into a new file in the log directory that a tab follows" )
            {
                REQUIRE( sessions.isSessionActive( device.devicePath() ) );
                REQUIRE( sessions.portOf( "board" ) == device.devicePath() );
                REQUIRE( host.followedFiles.size() == 1 );
                REQUIRE( QFileInfo( host.followedFiles.first() ).absolutePath()
                         == QFileInfo( logDir.path() ).absoluteFilePath() );
            }

            THEN( "the tile shows it running, with Stop, and cannot be deleted" )
            {
                const auto* tile = sidebar.tile( "board" );
                REQUIRE( stateOf( tile ) == "running" );
                REQUIRE( tile->findChild<QPushButton*>( "startStop" )->text().contains( "Stop" ) );
                REQUIRE_FALSE( tile->findChild<QPushButton*>( "delete" )->isVisible() );
                REQUIRE( textOf( tile, "details" ) == "running" );
            }

            THEN( "what the device sends goes to that file" )
            {
                REQUIRE( device.send( "boot ok\n" ) );
                const auto path = host.followedFiles.first();
                REQUIRE( waitFor( [ &path ] {
                    QFile file( path );
                    return file.open( QIODevice::ReadOnly ) && file.readAll() == "boot ok\n";
                } ) );
            }

            AND_WHEN( "its Stop is clicked" )
            {
                click( sidebar.tile( "board" ), "startStop" );

                THEN( "the capture ends, its file stays for its tab, and the tile is stopped" )
                {
                    REQUIRE_FALSE( sessions.isSessionActive( device.devicePath() ) );
                    REQUIRE( QFileInfo::exists( host.followedFiles.first() ) );
                    REQUIRE( stateOf( sidebar.tile( "board" ) ) == "stopped" );
                    REQUIRE( textOf( sidebar.tile( "board" ), "details" ).isEmpty() );
                }
            }

            AND_WHEN( "its port is changed in the configuration while it runs" )
            {
                auto changed = profileFor( "board", "/dev/logsquirl-elsewhere", logDir.path() );
                REQUIRE( profiles.store.upsert( changed ).isEmpty() );

                THEN( "the tile still shows its capture running, and Stop stops it" )
                {
                    REQUIRE( stateOf( sidebar.tile( "board" ) ) == "running" );
                    click( sidebar.tile( "board" ), "startStop" );
                    REQUIRE_FALSE( sessions.isSessionActive( device.devicePath() ) );
                }
            }

            AND_WHEN( "its configuration is deleted while it runs" )
            {
                const auto problem = profiles.store.remove( "board" );

                THEN( "it is refused: it must be stopped first" )
                {
                    REQUIRE( problem.contains( "stop" ) );
                    REQUIRE( sidebar.tile( "board" ) );
                }
            }
        }

        sessions.stopAll();
    }
}

SCENARIO( "tiles on different ports capture at the same time", "[sidebarwidget]" )
{
    GIVEN( "two tiles for two devices" )
    {
        FakeHost host;
        serial_test::PseudoTerminal first;
        serial_test::PseudoTerminal second;
        Sessions sessions;
        Store profiles( host, sessions );
        QTemporaryDir logDir;
        REQUIRE( profiles.store.upsert( profileFor( "board", first.devicePath(), logDir.path() ) )
                     .isEmpty() );
        REQUIRE(
            profiles.store.upsert( profileFor( "console", second.devicePath(), logDir.path() ) )
                .isEmpty() );
        SidebarWidget sidebar( &profiles.store, &sessions );
        sidebar.show();

        WHEN( "both are started" )
        {
            click( sidebar.tile( "board" ), "startStop" );
            click( sidebar.tile( "console" ), "startStop" );

            THEN( "both capture, each into its own followed tab" )
            {
                REQUIRE( sessions.activeSessionCount() == 2 );
                REQUIRE( stateOf( sidebar.tile( "board" ) ) == "running" );
                REQUIRE( stateOf( sidebar.tile( "console" ) ) == "running" );
                REQUIRE( host.followedFiles.size() == 2 );
                REQUIRE( host.followedFiles.at( 0 ) != host.followedFiles.at( 1 ) );
            }

            AND_WHEN( "one is stopped" )
            {
                click( sidebar.tile( "board" ), "startStop" );

                THEN( "the other runs on" )
                {
                    REQUIRE( stateOf( sidebar.tile( "board" ) ) == "stopped" );
                    REQUIRE( stateOf( sidebar.tile( "console" ) ) == "running" );
                    REQUIRE( sessions.isSessionActive( second.devicePath() ) );
                }
            }

            AND_WHEN( "Stop all is clicked" )
            {
                sidebar.findChild<QPushButton*>( "stopAll" )->click();

                THEN( "both stop" )
                {
                    REQUIRE( sessions.activeSessionCount() == 0 );
                    REQUIRE( stateOf( sidebar.tile( "board" ) ) == "stopped" );
                    REQUIRE( stateOf( sidebar.tile( "console" ) ) == "stopped" );
                    REQUIRE_FALSE( sidebar.findChild<QPushButton*>( "stopAll" )->isEnabled() );
                }
            }
        }

        sessions.stopAll();
    }
}

SCENARIO( "a second tile on a busy port is refused with a message", "[sidebarwidget]" )
{
    GIVEN( "two tiles for the same device, the first capturing" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions;
        Store profiles( host, sessions );
        QTemporaryDir logDir;
        REQUIRE( profiles.store.upsert( profileFor( "board", device.devicePath(), logDir.path() ) )
                     .isEmpty() );
        REQUIRE(
            profiles.store.upsert( profileFor( "console", device.devicePath(), logDir.path() ) )
                .isEmpty() );
        SidebarWidget sidebar( &profiles.store, &sessions );
        click( sidebar.tile( "board" ), "startStop" );
        REQUIRE( sessions.portOf( "board" ) == device.devicePath() );
        host.notifications.clear();
        host.openedFiles.clear();

        THEN( "the second tile says which configuration holds its port" )
        {
            REQUIRE( stateOf( sidebar.tile( "console" ) ) == "stopped" );
            REQUIRE( textOf( sidebar.tile( "console" ), "details" )
                     == device.devicePath() + " is in use by “board”" );
        }

        WHEN( "the second tile's Start is clicked" )
        {
            click( sidebar.tile( "console" ), "startStop" );

            THEN( "it is refused, with a notification that says why" )
            {
                REQUIRE( sessions.portOf( "console" ).isEmpty() );
                REQUIRE( sessions.ownerOf( device.devicePath() ) == "board" );
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "board" ) );
                REQUIRE( stateOf( sidebar.tile( "console" ) ) == "stopped" );
            }
        }

        WHEN( "the first is stopped" )
        {
            click( sidebar.tile( "board" ), "startStop" );

            THEN( "the second no longer says its port is in use, and starts" )
            {
                REQUIRE( textOf( sidebar.tile( "console" ), "details" ).isEmpty() );
                click( sidebar.tile( "console" ), "startStop" );
                REQUIRE( sessions.portOf( "console" ) == device.devicePath() );
            }
        }

        sessions.stopAll();
    }
}

SCENARIO( "a tile follows a capture that ends on its own", "[sidebarwidget][signals]" )
{
    GIVEN( "a capturing tile" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions;
        Store profiles( host, sessions );
        QTemporaryDir logDir;
        REQUIRE( profiles.store.upsert( profileFor( "board", device.devicePath(), logDir.path() ) )
                     .isEmpty() );
        SidebarWidget sidebar( &profiles.store, &sessions );
        click( sidebar.tile( "board" ), "startStop" );
        REQUIRE( stateOf( sidebar.tile( "board" ) ) == "running" );

        WHEN( "its device is unplugged" )
        {
            // A pseudo-terminal cannot be unplugged: the port reports it.
            auto* session = sessions.findChild<serial_monitor::SerialProcess*>();
            REQUIRE( session );
            QMetaObject::invokeMethod(
                session, "onPortError",
                Q_ARG( QSerialPort::SerialPortError, QSerialPort::ResourceError ) );

            THEN( "the tile shows it stopped" )
            {
                REQUIRE( waitFor(
                    [ &sidebar ] { return stateOf( sidebar.tile( "board" ) ) == "stopped"; } ) );
            }
        }

        sessions.stopAll();
    }
}
#endif
