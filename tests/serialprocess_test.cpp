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
 * Tests basic construction, property accessors, default configuration,
 * and configDir fallback without requiring real serial hardware.  The
 * scenarios that need an open port use a pseudo-terminal as the device
 * (see pseudoterminal.h) and only run on Unix.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "plugin.h"
#include "pseudoterminal.h"
#include "readonlydir.h"
#include "serialprocess.h"

#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

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

        SerialProcess proc( cfg );

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

SCENARIO( "defaultConfig returns sensible defaults", "[serialprocess]" )
{
    GIVEN( "a plugin config directory without saved defaults" )
    {
        FakeHost host;
        const auto cfg = SerialProcess::defaultConfig();

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
    }
}

SCENARIO( "defaultConfig applies the saved defaults", "[serialprocess]" )
{
    GIVEN( "defaults saved through the Configure dialog" )
    {
        FakeHost host;
        {
            QSettings settings( host.configDir() + "/serial.ini", QSettings::IniFormat );
            settings.setValue( "serial/defaultBaud", 9600 );
            settings.setValue( "serial/timestamps", false );
        }

        WHEN( "reading the default configuration" )
        {
            const auto cfg = SerialProcess::defaultConfig();

            THEN( "the saved baud rate and timestamp setting are used" )
            {
                REQUIRE( cfg.baudRate == 9600 );
                REQUIRE_FALSE( cfg.timestamps );
            }
        }
    }
}

SCENARIO( "configDir falls back to temp when plugin is not initialised", "[serialprocess]" )
{
    GIVEN( "an uninitialised plugin state" )
    {
        // Ensure the global state is cleared (it should be by default in tests)
        serial_monitor::g_state.api = nullptr;
        serial_monitor::g_state.handle = nullptr;

        WHEN( "configDir is called" )
        {
            const auto dir = SerialProcess::configDir();

            THEN( "a non-empty fallback path is returned" )
            {
                REQUIRE_FALSE( dir.isEmpty() );
            }
        }
    }
}

SCENARIO( "rotateLog does nothing while no session runs", "[serialprocess]" )
{
    GIVEN( "a SerialProcess that has not been started" )
    {
        SerialConfig cfg;
        cfg.portName = "/dev/ttyUSB0";
        SerialProcess proc( cfg );

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
        SerialProcess proc( configFor( kMissingPort ), savePath );
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

        SerialProcess proc( configFor( kMissingPort ), savePath );

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

        SerialProcess proc( configFor( device.devicePath() ) );

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

    GIVEN( "a device that ends its lines with a lone CR" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;

        SerialProcess proc( configFor( device.devicePath() ) );

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

        SerialProcess proc( configFor( device.devicePath() ), savePath );

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
            SerialProcess first( configFor( device.devicePath() ), savePath );
            REQUIRE( first.start() );
            REQUIRE( device.send( "one\n" ) );
            REQUIRE( waitFor( [ &first ]() { return first.lineCount() == 1; } ) );
            first.stop();

            SerialProcess second( configFor( device.devicePath() ), savePath );
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

#ifdef Q_OS_UNIX
SCENARIO( "rotateLog moves the capture to a new file", "[serialprocess]" )
{
    GIVEN( "a running session writing to a generated file in the log directory" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        QTemporaryDir logDir;
        const auto savePath = SerialProcess::generateLogPath( logDir.path(), device.devicePath() );

        SerialProcess proc( configFor( device.devicePath() ), savePath );
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

        SerialProcess proc( configFor( device.devicePath() ) );
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
        SerialProcess proc( configFor( device.devicePath() ), savePath );
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
SCENARIO( "destroying a running session does not call back its owner", "[serialprocess]" )
{
    GIVEN( "a running session with listeners" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        int signalCount = 0;
        auto* proc = new SerialProcess( configFor( device.devicePath() ) );
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
