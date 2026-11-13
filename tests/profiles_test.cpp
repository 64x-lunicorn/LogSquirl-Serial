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
 * @brief BDD tests for serial.ini: the defaults, the saved port
 *        configurations and the log directory, and their store.
 *
 * The store is tested on a temporary directory, as the configuration
 * directory, without a host; through the dialog, the sidebar and Configure
 * in portdialog_test.cpp, sidebarwidget_test.cpp and plugin_test.cpp.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "profiles.h"
#include "readonlydir.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using serial_monitor::Defaults;
using serial_monitor::PortProfile;
using serial_monitor::ProfileStore;

namespace {

/// Write @p text as the settings file in @p configDir, as a user's file
/// is on disk.
void writeIni( const QTemporaryDir& configDir, const QByteArray& text )
{
    QFile file( ProfileStore::settingsFileIn( configDir.path() ) );
    REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( file.write( text ) == text.size() );
}

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

} // namespace

SCENARIO( "a saved configuration survives a restart", "[profiles]" )
{
    GIVEN( "a store on the plugin's settings file" )
    {
        QTemporaryDir configDir;
        ProfileStore store( configDir.path() );
        REQUIRE( store.list().isEmpty() );

        WHEN( "a configuration is saved" )
        {
            REQUIRE( store.upsert( boardProfile() ).isEmpty() );

            THEN( "a store opened later on the same file finds every setting" )
            {
                const ProfileStore later( configDir.path() );
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
        QTemporaryDir configDir;
        ProfileStore store( configDir.path() );
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
        QTemporaryDir configDir;
        ProfileStore store( configDir.path(), []( const QString& name ) {
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

    GIVEN( "a store without a configuration directory" )
    {
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
        QTemporaryDir configDir;
        ProfileStore store( configDir.path() );
        Defaults defaults;
        defaults.baudRate = 9600;
        defaults.timestamps = false;
        REQUIRE( store.saveDefaults( defaults ).isEmpty() );
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
}

SCENARIO( "the defaults are saved in the configuration directory", "[profiles]" )
{
    GIVEN( "a configuration directory without saved defaults" )
    {
        QTemporaryDir configDir;
        ProfileStore store( configDir.path() );

        THEN( "the defaults are 115200 baud, timestamps on, and a new file at the default size" )
        {
            const auto defaults = store.defaults();
            REQUIRE( defaults.baudRate == 115200 );
            REQUIRE( defaults.timestamps );
            REQUIRE( defaults.maxFileMiB == serial_monitor::kDefaultMaxFileMiB );
            REQUIRE( defaults.maxFileBytes()
                     == serial_monitor::kDefaultMaxFileMiB * serial_monitor::kBytesPerMiB );
            REQUIRE( store.defaultsUnsavable().isEmpty() );
        }

        THEN( "its captures are kept in its logs folder" )
        {
            REQUIRE( store.settingsFile() == configDir.filePath( "serial.ini" ) );
            REQUIRE( store.defaultLogDir() == configDir.filePath( "logs" ) );
        }

        WHEN( "other defaults are saved" )
        {
            Defaults saved;
            saved.baudRate = 9600;
            saved.timestamps = false;
            saved.maxFileMiB = 5;
            REQUIRE( store.saveDefaults( saved ).isEmpty() );

            THEN( "a store opened later on the directory has them" )
            {
                const auto defaults = ProfileStore( configDir.path() ).defaults();
                REQUIRE( defaults.baudRate == 9600 );
                REQUIRE_FALSE( defaults.timestamps );
                REQUIRE( defaults.maxFileMiB == 5 );
            }

            THEN( "a capture is rotated at their size (sessions_test.cpp starts one)" )
            {
                REQUIRE( store.defaults().maxFileBytes() == 5 * 1024 * 1024 );
            }
        }

        WHEN( "the size limit is turned off" )
        {
            Defaults saved;
            saved.maxFileMiB = 0;
            REQUIRE( store.saveDefaults( saved ).isEmpty() );

            THEN( "log files are never rotated by size" )
            {
                REQUIRE( store.defaults().maxFileBytes() == 0 );
            }
        }

        WHEN( "defaults are saved next to a configuration" )
        {
            REQUIRE( store.upsert( boardProfile() ).isEmpty() );
            Defaults saved;
            saved.baudRate = 9600;
            REQUIRE( store.saveDefaults( saved ).isEmpty() );

            THEN( "each keeps the other" )
            {
                REQUIRE( store.find( "board" ) );
                REQUIRE( store.find( "board" )->serial.baudRate == boardProfile().serial.baudRate );
                REQUIRE( store.newProfile().logDir == boardProfile().logDir );
                REQUIRE( store.upsert( boardProfile() ).isEmpty() );
                REQUIRE( store.defaults().baudRate == 9600 );
            }
        }
    }

    GIVEN( "a store without a configuration directory" )
    {
        ProfileStore store( {} );

        THEN( "it falls back to the defaults, and has no default log directory" )
        {
            const auto defaults = store.defaults();
            REQUIRE( defaults.baudRate == Defaults().baudRate );
            REQUIRE( defaults.timestamps == Defaults().timestamps );
            REQUIRE( defaults.maxFileMiB == Defaults().maxFileMiB );
            REQUIRE( store.settingsFile().isEmpty() );
            REQUIRE( store.defaultLogDir().isEmpty() );
        }

        THEN( "it says why the defaults cannot be saved, and saves none" )
        {
            REQUIRE( store.defaultsUnsavable().contains( "no configuration directory" ) );
            Defaults saved;
            saved.baudRate = 9600;
            REQUIRE( store.saveDefaults( saved ) == store.defaultsUnsavable() );
            REQUIRE( store.defaults().baudRate == 115200 );
        }
    }

#ifdef Q_OS_UNIX
    GIVEN( "a configuration directory that cannot be written" )
    {
        if ( !serial_test::ReadOnlyDir::isEnforced() ) {
            WARN( "File permissions are not enforced (running as root?); skipped." );
            return;
        }
        QTemporaryDir configDir;
        ProfileStore store( configDir.path() );

        WHEN( "defaults are saved" )
        {
            Defaults saved;
            saved.timestamps = false;
            QString problem;
            {
                const serial_test::ReadOnlyDir readOnly( configDir.path() );
                problem = store.saveDefaults( saved );
            }

            THEN( "the store says they could not be saved, and where, and keeps the old ones" )
            {
                REQUIRE( problem.contains( "could not be saved" ) );
                REQUIRE( problem.contains( store.settingsFile() ) );
                REQUIRE( store.defaults().timestamps );
            }
        }
    }
#endif
}

SCENARIO( "a serial.ini of earlier versions loads as it did", "[profiles]" )
{
    GIVEN( "the serial.ini of 0.6.0: the defaults and the sidebar's log directory" )
    {
        QTemporaryDir configDir;
        writeIni( configDir, "[serial]\n"
                             "defaultBaud=9600\n"
                             "timestamps=false\n"
                             "logDir=/var/log/serial\n" );
        ProfileStore store( configDir.path() );

        THEN( "its defaults are loaded, with the default size limit, which it had not" )
        {
            const auto defaults = store.defaults();
            REQUIRE( defaults.baudRate == 9600 );
            REQUIRE_FALSE( defaults.timestamps );
            REQUIRE( defaults.maxFileMiB == serial_monitor::kDefaultMaxFileMiB );
        }

        THEN( "it has no configuration, and a new one proposes its defaults and log directory" )
        {
            REQUIRE( store.list().isEmpty() );
            const auto profile = store.newProfile();
            REQUIRE( profile.serial.baudRate == 9600 );
            REQUIRE_FALSE( profile.serial.timestamps );
            REQUIRE( profile.logDir == "/var/log/serial" );
        }
    }

    GIVEN( "a serial.ini with a size limit and saved configurations" )
    {
        QTemporaryDir configDir;
        writeIni( configDir, "[serial]\n"
                             "defaultBaud=57600\n"
                             "timestamps=true\n"
                             "maxFileMiB=50\n"
                             "logDir=/var/log/serial\n"
                             "\n"
                             "[profiles]\n"
                             "1\\name=board\n"
                             "1\\port=/dev/ttyUSB0\n"
                             "1\\baudRate=74880\n"
                             "1\\dataBits=7\n"
                             "1\\stopBits=2\n"
                             "1\\parity=even\n"
                             "1\\flowControl=rtscts\n"
                             "1\\timestamps=false\n"
                             "1\\logDir=/var/log/board\n"
                             "2\\name=console\n"
                             "2\\port=COM3\n"
                             "2\\baudRate=9600\n"
                             "2\\dataBits=8\n"
                             "2\\stopBits=1.5\n"
                             "2\\parity=odd\n"
                             "2\\flowControl=xonxoff\n"
                             "2\\timestamps=true\n"
                             "2\\logDir=\n"
                             "size=2\n" );
        ProfileStore store( configDir.path() );

        THEN( "the defaults and every setting of each configuration are loaded, in order" )
        {
            const auto defaults = store.defaults();
            REQUIRE( defaults.baudRate == 57600 );
            REQUIRE( defaults.timestamps );
            REQUIRE( defaults.maxFileMiB == 50 );

            const auto profiles = store.list();
            REQUIRE( namesOf( profiles ) == QStringList{ "board", "console" } );
            const auto& board = profiles.at( 0 );
            REQUIRE( board.serial.portName == "/dev/ttyUSB0" );
            REQUIRE( board.serial.baudRate == 74880 );
            REQUIRE( board.serial.dataBits == QSerialPort::Data7 );
            REQUIRE( board.serial.stopBits == QSerialPort::TwoStop );
            REQUIRE( board.serial.parity == QSerialPort::EvenParity );
            REQUIRE( board.serial.flowControl == QSerialPort::HardwareControl );
            REQUIRE_FALSE( board.serial.timestamps );
            REQUIRE( board.logDir == "/var/log/board" );
            const auto& console = profiles.at( 1 );
            REQUIRE( console.serial.portName == "COM3" );
            REQUIRE( console.serial.baudRate == 9600 );
            REQUIRE( console.serial.dataBits == QSerialPort::Data8 );
            REQUIRE( console.serial.stopBits == QSerialPort::OneAndHalfStop );
            REQUIRE( console.serial.parity == QSerialPort::OddParity );
            REQUIRE( console.serial.flowControl == QSerialPort::SoftwareControl );
            REQUIRE( console.serial.timestamps );
            REQUIRE( console.logDir.isEmpty() );
            REQUIRE( store.newProfile().logDir == "/var/log/serial" );
        }
    }
}

SCENARIO( "settings that are no settings load as the defaults", "[profiles]" )
{
    GIVEN( "a configuration with a broken entry for each setting" )
    {
        QTemporaryDir configDir;
        writeIni( configDir, "[profiles]\n"
                             "1\\name=broken\n"
                             "1\\port=COM3\n"
                             "1\\baudRate=fast\n"
                             "1\\dataBits=9\n"
                             "1\\stopBits=3\n"
                             "1\\parity=sometimes\n"
                             "1\\flowControl=both\n"
                             "size=1\n" );
        ProfileStore store( configDir.path() );

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

    for ( const auto& [ baud, maxFileMiB ] :
          { std::pair<QByteArray, QByteArray>{ "fast", "big" }, { "0", "-1" }, { "-9600", "" } } ) {
        GIVEN( "defaults with the baud rate " + baud.toStdString() + " and the size limit "
               + maxFileMiB.toStdString() )
        {
            QTemporaryDir configDir;
            writeIni( configDir,
                      "[serial]\ndefaultBaud=" + baud + "\nmaxFileMiB=" + maxFileMiB + "\n" );
            ProfileStore store( configDir.path() );

            THEN( "they load as the built-in baud rate and size limit" )
            {
                REQUIRE( store.defaults().baudRate == Defaults().baudRate );
                REQUIRE( store.defaults().maxFileMiB == Defaults().maxFileMiB );
                REQUIRE( store.newProfile().serial.baudRate == 115200 );
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
