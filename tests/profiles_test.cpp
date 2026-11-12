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
 * @file profiles_test.cpp
 * @brief BDD tests for the saved port configurations and their store.
 *
 * The store is tested on the configuration directory of a FakeHost, the
 * file the plugin uses (serial.ini); through the dialog and the sidebar in
 * portdialog_test.cpp, sidebarwidget_test.cpp and plugin_test.cpp.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "profiles.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

using serial_monitor::PortProfile;
using serial_monitor::ProfileStore;
using serial_test::FakeHost;

namespace {

/// A configuration with every setting off its default.
PortProfile boardProfile()
{
    PortProfile profile;
    profile.name = "board";
    profile.serial.portName = "/dev/ttyUSB0";
    profile.serial.baudRate = 74880;
    profile.serial.dataBits = QSerialPort::Data7;
    profile.serial.stopBits = QSerialPort::TwoStop;
    profile.serial.parity = QSerialPort::EvenParity;
    profile.serial.flowControl = QSerialPort::HardwareControl;
    profile.serial.timestamps = false;
    profile.logDir = QDir::homePath() + "/serial-logs";
    return profile;
}

QStringList namesOf( const QList<PortProfile>& profiles )
{
    QStringList names;
    for ( const auto& profile : profiles ) {
        names << profile.name;
    }
    return names;
}

QString iniOf( const FakeHost& host )
{
    return host.configDir() + "/serial.ini";
}

} // namespace

SCENARIO( "a saved configuration survives a restart", "[profiles]" )
{
    GIVEN( "a store on the plugin's settings file" )
    {
        FakeHost host;
        ProfileStore store( iniOf( host ) );
        REQUIRE( store.list().isEmpty() );

        WHEN( "a configuration is saved" )
        {
            REQUIRE( store.upsert( boardProfile() ).isEmpty() );

            THEN( "a store opened later on the same file finds every setting" )
            {
                const ProfileStore later( iniOf( host ) );
                const auto found = later.find( "board" );
                REQUIRE( found );
                const auto expected = boardProfile();
                REQUIRE( found->serial.portName == expected.serial.portName );
                REQUIRE( found->serial.baudRate == expected.serial.baudRate );
                REQUIRE( found->serial.dataBits == expected.serial.dataBits );
                REQUIRE( found->serial.stopBits == expected.serial.stopBits );
                REQUIRE( found->serial.parity == expected.serial.parity );
                REQUIRE( found->serial.flowControl == expected.serial.flowControl );
                REQUIRE( found->serial.timestamps == expected.serial.timestamps );
                REQUIRE( found->logDir == expected.logDir );
            }
        }
    }
}

SCENARIO( "configurations are saved, replaced and deleted by name", "[profiles]" )
{
    GIVEN( "a store with two configurations" )
    {
        FakeHost host;
        ProfileStore store( iniOf( host ) );
        auto first = boardProfile();
        auto second = boardProfile();
        second.name = "console";
        second.serial.portName = "/dev/ttyUSB1";
        REQUIRE( store.upsert( first ).isEmpty() );
        REQUIRE( store.upsert( second ).isEmpty() );
        int changed = 0;
        QObject::connect( &store, &ProfileStore::changed, [ &changed ] { ++changed; } );

        THEN( "they are listed in the order they were saved" )
        {
            REQUIRE( namesOf( store.list() ) == QStringList{ "board", "console" } );
        }

        WHEN( "the first is saved again with another port" )
        {
            first.serial.portName = "/dev/ttyACM0";
            REQUIRE( store.upsert( first ).isEmpty() );

            THEN( "it is replaced where it was, and the change announced" )
            {
                REQUIRE( namesOf( store.list() ) == QStringList{ "board", "console" } );
                REQUIRE( store.find( "board" )->serial.portName == "/dev/ttyACM0" );
                REQUIRE( changed == 1 );
            }
        }

        WHEN( "the first is deleted" )
        {
            REQUIRE( store.remove( "board" ).isEmpty() );

            THEN( "only the other is left, and the change announced" )
            {
                REQUIRE( namesOf( store.list() ) == QStringList{ "console" } );
                REQUIRE_FALSE( store.find( "board" ) );
                REQUIRE( changed == 1 );
            }
        }

        WHEN( "a configuration that is not saved is deleted" )
        {
            const auto problem = store.remove( "nothing" );

            THEN( "the store says so, and nothing changes" )
            {
                REQUIRE( problem.contains( "nothing" ) );
                REQUIRE( changed == 0 );
            }
        }

        WHEN( "a configuration without a name is saved" )
        {
            auto unnamed = boardProfile();
            unnamed.name.clear();
            const auto problem = store.upsert( unnamed );

            THEN( "it is refused with a reason" )
            {
                REQUIRE( problem.contains( "name" ) );
                REQUIRE( store.list().size() == 2 );
            }
        }

        WHEN( "the first is saved under a new name, with another port" )
        {
            auto renamed = first;
            renamed.name = "main";
            renamed.serial.portName = "/dev/ttyACM0";
            REQUIRE( store.rename( "board", renamed ).isEmpty() );

            THEN( "it is renamed where it was, with its change, and the change announced once" )
            {
                REQUIRE( namesOf( store.list() ) == QStringList{ "main", "console" } );
                REQUIRE( store.find( "main" )->serial.portName == "/dev/ttyACM0" );
                REQUIRE( changed == 1 );
            }
        }

        WHEN( "the first is saved under its own name through rename" )
        {
            first.serial.portName = "/dev/ttyACM0";
            REQUIRE( store.rename( "board", first ).isEmpty() );

            THEN( "it is saved as by upsert" )
            {
                REQUIRE( namesOf( store.list() ) == QStringList{ "board", "console" } );
                REQUIRE( store.find( "board" )->serial.portName == "/dev/ttyACM0" );
            }
        }

        WHEN( "the first is renamed to the name of the second" )
        {
            auto renamed = first;
            renamed.name = "console";
            const auto problem = store.rename( "board", renamed );

            THEN( "it is refused, saying why, and nothing changes" )
            {
                REQUIRE( problem.contains( "already" ) );
                REQUIRE( namesOf( store.list() ) == QStringList{ "board", "console" } );
                REQUIRE( store.find( "console" )->serial.portName == "/dev/ttyUSB1" );
                REQUIRE( changed == 0 );
            }
        }

        WHEN( "a configuration that is not saved is renamed" )
        {
            auto renamed = first;
            renamed.name = "main";
            const auto problem = store.rename( "nothing", renamed );

            THEN( "the store says so, and nothing changes" )
            {
                REQUIRE( problem.contains( "nothing" ) );
                REQUIRE( changed == 0 );
            }
        }
    }

    GIVEN( "a store that knows which configurations capture" )
    {
        FakeHost host;
        ProfileStore store( iniOf( host ), []( const QString& name ) {
            return name == QStringLiteral( "board" );
        } );
        REQUIRE( store.upsert( boardProfile() ).isEmpty() );

        WHEN( "the capturing configuration is deleted" )
        {
            const auto problem = store.remove( "board" );

            THEN( "it is refused: it must be stopped first" )
            {
                REQUIRE( problem.contains( "stop" ) );
                REQUIRE( store.find( "board" ) );
            }
        }

        WHEN( "the capturing configuration is renamed" )
        {
            auto renamed = boardProfile();
            renamed.name = "main";
            const auto problem = store.rename( "board", renamed );

            THEN( "it is refused: its capture is known by its name, it must be stopped first" )
            {
                REQUIRE( problem.contains( "stop" ) );
                REQUIRE( store.find( "board" ) );
                REQUIRE_FALSE( store.find( "main" ) );
            }
        }
    }

    GIVEN( "a store without a settings file, as without a configuration directory" )
    {
        FakeHost host;
        ProfileStore store( {} );

        WHEN( "a configuration is saved" )
        {
            const auto problem = store.upsert( boardProfile() );

            THEN( "it is refused, saying why" )
            {
                REQUIRE( problem.contains( "configuration directory" ) );
                REQUIRE( store.list().isEmpty() );
            }
        }
    }
}

SCENARIO( "a new configuration proposes the defaults", "[profiles]" )
{
    GIVEN( "defaults saved in Configure and a configuration saved with a log directory" )
    {
        FakeHost host;
        {
            QSettings settings( iniOf( host ), QSettings::IniFormat );
            settings.setValue( "serial/defaultBaud", 9600 );
            settings.setValue( "serial/timestamps", false );
        }
        ProfileStore store( iniOf( host ) );
        REQUIRE( store.upsert( boardProfile() ).isEmpty() );

        WHEN( "a new configuration is set up" )
        {
            const auto profile = store.newProfile();

            THEN( "it has the default baud rate and timestamps, and the log directory saved last" )
            {
                REQUIRE( profile.name.isEmpty() );
                REQUIRE( profile.serial.portName.isEmpty() );
                REQUIRE( profile.serial.baudRate == 9600 );
                REQUIRE_FALSE( profile.serial.timestamps );
                REQUIRE( profile.serial.dataBits == QSerialPort::Data8 );
                REQUIRE( profile.logDir == boardProfile().logDir );
            }
        }
    }

    GIVEN( "the log directory of the sidebar of earlier versions" )
    {
        FakeHost host;
        {
            QSettings settings( iniOf( host ), QSettings::IniFormat );
            settings.setValue( "serial/logDir", "/var/log/serial" );
        }
        ProfileStore store( iniOf( host ) );

        THEN( "a new configuration proposes it" )
        {
            REQUIRE( store.newProfile().logDir == "/var/log/serial" );
        }
    }
}

SCENARIO( "settings that are no settings load as the defaults", "[profiles]" )
{
    GIVEN( "a configuration with a broken entry for each setting" )
    {
        FakeHost host;
        {
            QSettings settings( iniOf( host ), QSettings::IniFormat );
            settings.beginWriteArray( "profiles" );
            settings.setArrayIndex( 0 );
            settings.setValue( "name", "broken" );
            settings.setValue( "port", "COM3" );
            settings.setValue( "baudRate", "fast" );
            settings.setValue( "dataBits", 9 );
            settings.setValue( "stopBits", "3" );
            settings.setValue( "parity", "sometimes" );
            settings.setValue( "flowControl", "both" );
            settings.endArray();
        }
        ProfileStore store( iniOf( host ) );

        WHEN( "it is loaded" )
        {
            const auto profile = store.find( "broken" );

            THEN( "each broken setting is its default" )
            {
                REQUIRE( profile );
                const serial_monitor::SerialConfig defaults;
                REQUIRE( profile->serial.portName == "COM3" );
                REQUIRE( profile->serial.baudRate == defaults.baudRate );
                REQUIRE( profile->serial.dataBits == defaults.dataBits );
                REQUIRE( profile->serial.stopBits == defaults.stopBits );
                REQUIRE( profile->serial.parity == defaults.parity );
                REQUIRE( profile->serial.flowControl == defaults.flowControl );
                REQUIRE( profile->serial.timestamps == defaults.timestamps );
            }
        }
    }
}

SCENARIO( "a configuration is summed up for its tile", "[profiles]" )
{
    GIVEN( "the usual settings" )
    {
        PortProfile profile;
        profile.serial.portName = "COM3";

        THEN( "they read 115200 8N1, without flow control" )
        {
            REQUIRE( serial_monitor::frameOf( profile.serial ) == "115200 8N1" );
            REQUIRE( serial_monitor::flowControlName( profile.serial.flowControl )
                     == "no flow control" );
        }
    }

    GIVEN( "every setting off its default" )
    {
        const auto profile = boardProfile();

        THEN( "they read as on a terminal's status line" )
        {
            REQUIRE( serial_monitor::frameOf( profile.serial ) == "74880 7E2" );
            REQUIRE( serial_monitor::flowControlName( profile.serial.flowControl ) == "RTS/CTS" );
        }
    }

    GIVEN( "one and a half stop bits, odd parity and XON/XOFF" )
    {
        PortProfile profile;
        profile.serial.baudRate = 9600;
        profile.serial.parity = QSerialPort::OddParity;
        profile.serial.stopBits = QSerialPort::OneAndHalfStop;
        profile.serial.flowControl = QSerialPort::SoftwareControl;

        THEN( "they read 9600 8O1.5 and XON/XOFF" )
        {
            REQUIRE( serial_monitor::frameOf( profile.serial ) == "9600 8O1.5" );
            REQUIRE( serial_monitor::flowControlName( profile.serial.flowControl ) == "XON/XOFF" );
        }
    }
}

SCENARIO( "a configuration says what keeps it from starting", "[profiles]" )
{
    GIVEN( "a configuration with a port" )
    {
        auto profile = boardProfile();

        THEN( "nothing keeps it from starting" )
        {
            REQUIRE( serial_monitor::startProblem( profile ).isEmpty() );
        }

        WHEN( "it has no port" )
        {
            profile.serial.portName.clear();

            THEN( "it asks for one" )
            {
                REQUIRE( serial_monitor::startProblem( profile ).contains( "port" ) );
            }
        }

        WHEN( "its baud rate is none" )
        {
            profile.serial.baudRate = 0;

            THEN( "it asks for one" )
            {
                REQUIRE( serial_monitor::startProblem( profile ).contains( "baud rate" ) );
            }
        }

        WHEN( "its log directory is not a full path" )
        {
            profile.logDir = "logs";

            THEN( "it says so" )
            {
                REQUIRE( serial_monitor::startProblem( profile ).contains( "full path" ) );
            }
        }
    }
}

SCENARIO( "a configuration's capture goes to its log directory", "[profiles]" )
{
    GIVEN( "a host with a configuration directory" )
    {
        FakeHost host;
        PortProfile profile;
        profile.serial.portName = "/dev/ttyUSB0";

        WHEN( "the configuration has no log directory" )
        {
            const auto path = serial_monitor::savePathFor( profile );

            THEN( "the capture goes to the logs folder of the configuration directory" )
            {
                REQUIRE( QFileInfo( path ).absolutePath()
                         == QDir( host.configDir() ).filePath( "logs" ) );
                REQUIRE( QFileInfo( path ).fileName().endsWith( "_ttyUSB0.log" ) );
            }
        }

        WHEN( "the configuration has a log directory that does not exist yet" )
        {
            profile.logDir = host.tempRoot() + "/captures";
            const auto path = serial_monitor::savePathFor( profile );

            THEN( "the directory is created, and the capture goes there" )
            {
                REQUIRE( QFileInfo( path ).absolutePath() == profile.logDir );
                REQUIRE( QFileInfo( profile.logDir ).isDir() );
            }
        }
    }

    GIVEN( "a host without a configuration directory" )
    {
        FakeHost host;
        host.hasConfigDir = false;
        PortProfile profile;
        profile.serial.portName = "/dev/ttyUSB0";

        THEN( "a configuration without a log directory captures to a temporary file" )
        {
            REQUIRE( serial_monitor::savePathFor( profile ).isEmpty() );
        }
    }
}
