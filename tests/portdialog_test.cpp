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
 * @file portdialog_test.cpp
 * @brief BDD tests for the dialog that edits the saved port configurations.
 *
 * The dialog is driven through its named children against a FakeHost,
 * with the sidebar beside it where a test checks the tiles.  The store
 * itself is tested in profiles_test.cpp.
 */

#include <catch2/catch.hpp>

#include "baudrate.h"
#include "connectiontile.h"
#include "fakehost.h"
#include "portdialog.h"
#include "profiles.h"
#include "pseudoterminal.h"
#include "sessions.h"
#include "sidebarwidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

using serial_monitor::PortDialog;
using serial_monitor::PortProfile;
using serial_monitor::ProfileStore;
using serial_monitor::Sessions;
using serial_monitor::SidebarWidget;
using serial_test::FakeHost;

namespace {

/// The dialog's and the sidebar's world: sessions, the store, both views.
struct World {
    explicit World( const FakeHost& host )
        : profiles( host.configDir() + "/serial.ini",
                    [ this ]( const QString& name ) { return !sessions.portOf( name ).isEmpty(); } )
        , dialog( &profiles, &sessions )
        , sidebar( &profiles, &sessions )
    {
    }

    Sessions sessions;
    ProfileStore profiles;
    PortDialog dialog;
    SidebarWidget sidebar;
};

template <typename Widget>
Widget* child( const PortDialog& dialog, const char* name )
{
    auto* widget = dialog.findChild<Widget*>( name );
    REQUIRE( widget );
    return widget;
}

/// Enter a configuration named @p name for @p port in the dialog.
void enter( PortDialog& dialog, const QString& name, const QString& port )
{
    child<QComboBox>( dialog, "profile" )->setEditText( name );
    child<QComboBox>( dialog, "port" )->setEditText( port );
}

void save( PortDialog& dialog )
{
    child<QPushButton>( dialog, "saveProfile" )->click();
}

QString statusOf( const PortDialog& dialog )
{
    return child<QLabel>( dialog, "status" )->text();
}

} // namespace

SCENARIO( "saving in the dialog creates a tile, editing updates it, deleting removes it",
          "[portdialog]" )
{
    GIVEN( "the dialog and the sidebar, without saved configurations" )
    {
        FakeHost host;
        World world( host );

        WHEN( "a configuration is entered and saved" )
        {
            world.dialog.newConfiguration();
            enter( world.dialog, "board", "/dev/ttyUSB0" );
            serial_monitor::selectBaudRate( child<QComboBox>( world.dialog, "baudRate" ), 9600 );
            auto* parity = child<QComboBox>( world.dialog, "parity" );
            parity->setCurrentIndex( parity->findData( QSerialPort::EvenParity ) );
            auto* dataBits = child<QComboBox>( world.dialog, "dataBits" );
            dataBits->setCurrentIndex( dataBits->findData( QSerialPort::Data7 ) );
            auto* flow = child<QComboBox>( world.dialog, "flowControl" );
            flow->setCurrentIndex( flow->findData( QSerialPort::SoftwareControl ) );
            child<QCheckBox>( world.dialog, "timestamps" )->setChecked( false );
            child<QLineEdit>( world.dialog, "logDir" )->setText( host.tempRoot() + "/logs" );
            save( world.dialog );

            THEN( "it is saved with every setting, and the dialog says where to start it" )
            {
                const auto saved = world.profiles.find( "board" );
                REQUIRE( saved );
                REQUIRE( saved->serial.portName == "/dev/ttyUSB0" );
                REQUIRE( saved->serial.baudRate == 9600 );
                REQUIRE( saved->serial.parity == QSerialPort::EvenParity );
                REQUIRE( saved->serial.dataBits == QSerialPort::Data7 );
                REQUIRE( saved->serial.flowControl == QSerialPort::SoftwareControl );
                REQUIRE_FALSE( saved->serial.timestamps );
                REQUIRE( saved->logDir == host.tempRoot() + "/logs" );
                REQUIRE( statusOf( world.dialog ).contains( "sidebar" ) );
            }

            THEN( "the sidebar has a tile for it" )
            {
                const auto* tile = world.sidebar.tile( "board" );
                REQUIRE( tile );
                REQUIRE( tile->findChild<QLabel*>( "summary" )->text()
                         == "/dev/ttyUSB0\n9600 7E1 · XON/XOFF" );
            }

            AND_WHEN( "it is opened for editing, changed and saved" )
            {
                world.dialog.newConfiguration();
                world.dialog.editConfiguration( "board" );
                REQUIRE( child<QComboBox>( world.dialog, "port" )->currentText()
                         == "/dev/ttyUSB0" );
                REQUIRE(
                    serial_monitor::baudRateFrom( child<QComboBox>( world.dialog, "baudRate" ) )
                    == 9600 );
                child<QComboBox>( world.dialog, "port" )->setEditText( "/dev/ttyACM0" );
                save( world.dialog );

                THEN( "its tile shows the change, and no second tile appears" )
                {
                    REQUIRE( world.profiles.list().size() == 1 );
                    REQUIRE( world.sidebar.tile( "board" )
                                 ->findChild<QLabel*>( "summary" )
                                 ->text()
                                 .startsWith( "/dev/ttyACM0\n" ) );
                }
            }

            AND_WHEN( "it is deleted in the dialog" )
            {
                world.dialog.editConfiguration( "board" );
                child<QPushButton>( world.dialog, "deleteProfile" )->click();

                THEN( "it and its tile are gone, and the dialog says so" )
                {
                    REQUIRE_FALSE( world.profiles.find( "board" ) );
                    REQUIRE_FALSE( world.sidebar.tile( "board" ) );
                    REQUIRE( statusOf( world.dialog ).contains( "deleted" ) );
                }
            }
        }

        WHEN( "a configuration without a name is saved" )
        {
            world.dialog.newConfiguration();
            enter( world.dialog, "", "/dev/ttyUSB0" );
            save( world.dialog );

            THEN( "nothing is saved, and the dialog asks for a name" )
            {
                REQUIRE( world.profiles.list().isEmpty() );
                REQUIRE( statusOf( world.dialog ).contains( "name" ) );
            }
        }

        WHEN( "a configuration without a port is saved" )
        {
            world.dialog.newConfiguration();
            enter( world.dialog, "board", "" );
            save( world.dialog );

            THEN( "nothing is saved, and the dialog asks for the port" )
            {
                REQUIRE( world.profiles.list().isEmpty() );
                REQUIRE( statusOf( world.dialog ).contains( "port" ) );
            }
        }

        WHEN( "a configuration with a relative log directory is saved" )
        {
            world.dialog.newConfiguration();
            enter( world.dialog, "board", "/dev/ttyUSB0" );
            child<QLineEdit>( world.dialog, "logDir" )->setText( "logs" );
            save( world.dialog );

            THEN( "nothing is saved, and the dialog says it is not a full path" )
            {
                REQUIRE( world.profiles.list().isEmpty() );
                REQUIRE( statusOf( world.dialog ).contains( "full path" ) );
            }
        }
    }
}

SCENARIO( "a new name saved in the dialog renames the configuration it edits", "[portdialog]" )
{
    GIVEN( "two saved configurations, the first opened for editing" )
    {
        FakeHost host;
        World world( host );
        world.dialog.newConfiguration();
        enter( world.dialog, "board", "/dev/ttyUSB0" );
        save( world.dialog );
        world.dialog.newConfiguration();
        enter( world.dialog, "console", "/dev/ttyUSB1" );
        save( world.dialog );
        world.dialog.newConfiguration();
        world.dialog.editConfiguration( "board" );

        WHEN( "it is given a new name and saved" )
        {
            child<QComboBox>( world.dialog, "profile" )->setEditText( "main" );
            child<QComboBox>( world.dialog, "port" )->setEditText( "/dev/ttyACM0" );
            save( world.dialog );

            THEN( "the configuration is renamed where it was, with its changes" )
            {
                QStringList names;
                for ( const auto& profile : world.profiles.list() ) {
                    names << profile.name;
                }
                REQUIRE( names == QStringList{ "main", "console" } );
                REQUIRE( world.profiles.find( "main" )->serial.portName == "/dev/ttyACM0" );
                REQUIRE( statusOf( world.dialog ).contains( "main" ) );
            }

            THEN( "its tile has the new name, and no tile is left with the old one" )
            {
                REQUIRE( world.sidebar.tile( "main" ) );
                REQUIRE_FALSE( world.sidebar.tile( "board" ) );
                REQUIRE( world.sidebar.findChildren<serial_monitor::ConnectionTile*>().size()
                         == 2 );
            }

            AND_WHEN( "it is renamed once more" )
            {
                child<QComboBox>( world.dialog, "profile" )->setEditText( "primary" );
                save( world.dialog );

                THEN( "the renamed configuration is renamed again" )
                {
                    REQUIRE( world.profiles.list().size() == 2 );
                    REQUIRE( world.profiles.find( "primary" ) );
                    REQUIRE_FALSE( world.profiles.find( "main" ) );
                }
            }
        }

        WHEN( "it is given the name of the other configuration and saved" )
        {
            child<QComboBox>( world.dialog, "profile" )->setEditText( "console" );
            child<QComboBox>( world.dialog, "port" )->setEditText( "/dev/ttyACM0" );
            save( world.dialog );

            THEN( "it is refused, both stay as they were, and the dialog says why" )
            {
                REQUIRE( world.profiles.list().size() == 2 );
                REQUIRE( world.profiles.find( "board" )->serial.portName == "/dev/ttyUSB0" );
                REQUIRE( world.profiles.find( "console" )->serial.portName == "/dev/ttyUSB1" );
                REQUIRE( statusOf( world.dialog ).contains( "already" ) );
            }
        }
    }

    GIVEN( "a new configuration set up in the dialog" )
    {
        FakeHost host;
        World world( host );
        world.dialog.newConfiguration();
        enter( world.dialog, "board", "/dev/ttyUSB0" );
        save( world.dialog );

        WHEN( "New configuration… sets up another, which is saved" )
        {
            world.dialog.newConfiguration();
            enter( world.dialog, "console", "/dev/ttyUSB1" );
            save( world.dialog );

            THEN( "both are saved: a new configuration renames none" )
            {
                REQUIRE( world.profiles.find( "board" ) );
                REQUIRE( world.profiles.find( "console" ) );
            }
        }
    }
}

SCENARIO( "the dialog follows configurations deleted from their tile", "[portdialog]" )
{
    GIVEN( "a dialog listing a saved configuration" )
    {
        FakeHost host;
        World world( host );
        PortProfile profile;
        profile.name = "board";
        profile.serial.portName = "/dev/ttyUSB0";
        REQUIRE( world.profiles.upsert( profile ).isEmpty() );
        auto* names = child<QComboBox>( world.dialog, "profile" );
        REQUIRE( names->findText( "board" ) >= 0 );

        WHEN( "it is deleted elsewhere" )
        {
            REQUIRE( world.profiles.remove( "board" ).isEmpty() );

            THEN( "the dialog no longer lists it" )
            {
                REQUIRE( names->findText( "board" ) < 0 );
            }
        }
    }
}

SCENARIO( "the dialog shares the sessions' port list", "[portdialog]" )
{
    GIVEN( "sessions that have scanned the ports" )
    {
        FakeHost host;
        Sessions sessions;
        ProfileStore profiles( host.configDir() + "/serial.ini" );
        const auto scansBefore = host.logs.filter( "Discovered" ).size();

        WHEN( "the dialog is created" )
        {
            PortDialog dialog( &profiles, &sessions );

            THEN( "it offers the ports found, without enumerating them again" )
            {
                REQUIRE( host.logs.filter( "Discovered" ).size() == scansBefore );
                REQUIRE( child<QComboBox>( dialog, "port" )->count() == sessions.ports().size() );
            }

            AND_WHEN( "the user types a port and clicks Refresh" )
            {
                child<QComboBox>( dialog, "port" )->setEditText( "/dev/logsquirl-typed" );
                child<QPushButton>( dialog, "refresh" )->click();

                THEN( "the ports are enumerated once, and the typed port is kept" )
                {
                    REQUIRE( host.logs.filter( "Discovered" ).size() == scansBefore + 1 );
                    REQUIRE( child<QComboBox>( dialog, "port" )->currentText()
                             == "/dev/logsquirl-typed" );
                }
            }
        }
    }
}

SCENARIO( "the empty log directory says where captures go", "[portdialog]" )
{
    GIVEN( "a host with a configuration directory" )
    {
        FakeHost host;
        Sessions sessions;
        ProfileStore profiles( host.configDir() + "/serial.ini" );
        PortDialog dialog( &profiles, &sessions );
        auto* logDir = child<QLineEdit>( dialog, "logDir" );

        THEN( "the empty field shows the logs directory in it, where they are kept" )
        {
            REQUIRE( logDir->text().isEmpty() );
            REQUIRE( logDir->placeholderText().contains(
                QDir::toNativeSeparators( QDir( host.configDir() ).filePath( "logs" ) ) ) );
            REQUIRE_FALSE( logDir->placeholderText().contains( "temp" ) );
        }
    }

    GIVEN( "a host without a configuration directory" )
    {
        FakeHost host;
        host.hasConfigDir = false;
        Sessions sessions;
        ProfileStore profiles( {} );
        PortDialog dialog( &profiles, &sessions );

        THEN( "the empty field says that captures are temporary" )
        {
            REQUIRE(
                child<QLineEdit>( dialog, "logDir" )->placeholderText().contains( "temporary" ) );
        }

        WHEN( "a configuration is saved" )
        {
            enter( dialog, "board", "/dev/ttyUSB0" );
            save( dialog );

            THEN( "the dialog says that it cannot be saved, and why" )
            {
                REQUIRE( statusOf( dialog ).contains( "configuration directory" ) );
            }
        }
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "saving a configuration that captures says the capture runs on", "[portdialog]" )
{
    GIVEN( "a configuration whose capture runs" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        World world( host );
        world.dialog.newConfiguration();
        enter( world.dialog, "board", device.devicePath() );
        child<QLineEdit>( world.dialog, "logDir" )->setText( host.tempRoot() + "/logs" );
        save( world.dialog );
        world.sidebar.tile( "board" )->findChild<QPushButton*>( "startStop" )->click();
        REQUIRE( world.sessions.portOf( "board" ) == device.devicePath() );

        WHEN( "it is changed and saved" )
        {
            world.dialog.editConfiguration( "board" );
            child<QCheckBox>( world.dialog, "timestamps" )->setChecked( true );
            save( world.dialog );

            THEN( "the dialog says the capture runs on with the settings it started with" )
            {
                REQUIRE( statusOf( world.dialog ).contains( "runs on" ) );
                REQUIRE( world.sessions.portOf( "board" ) == device.devicePath() );
            }
        }

        WHEN( "it is deleted in the dialog" )
        {
            world.dialog.editConfiguration( "board" );
            child<QPushButton>( world.dialog, "deleteProfile" )->click();

            THEN( "it is refused, and the dialog says to stop it first" )
            {
                REQUIRE( world.profiles.find( "board" ) );
                REQUIRE( statusOf( world.dialog ).contains( "stop" ) );
            }
        }

        WHEN( "it is given a new name in the dialog and saved" )
        {
            world.dialog.editConfiguration( "board" );
            child<QComboBox>( world.dialog, "profile" )->setEditText( "main" );
            save( world.dialog );

            THEN( "it is refused, and the dialog says to stop it first" )
            {
                REQUIRE( world.profiles.list().size() == 1 );
                REQUIRE( world.profiles.find( "board" ) );
                REQUIRE( statusOf( world.dialog ).contains( "stop" ) );
            }

            THEN( "its tile keeps its running capture" )
            {
                REQUIRE( world.sessions.portOf( "board" ) == device.devicePath() );
                REQUIRE( world.sidebar.tile( "board" )
                             ->findChild<QLabel*>( "state" )
                             ->property( "state" )
                             .toString()
                         == "running" );
            }
        }

        world.sessions.stopAll();
    }
}
#endif
