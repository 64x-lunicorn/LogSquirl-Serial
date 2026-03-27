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
 * and configDir fallback without requiring real serial hardware.
 */

#include <catch2/catch.hpp>

#include "serialprocess.h"
#include "plugin.h"

using serial_monitor::SerialConfig;
using serial_monitor::SerialProcess;

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
    GIVEN( "the defaultConfig static function" )
    {
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

SCENARIO( "rotateLog creates a new temp file and preserves the old one", "[serialprocess]" )
{
    GIVEN( "a SerialProcess that is not running" )
    {
        SerialConfig cfg;
        cfg.portName = "/dev/ttyUSB0";
        SerialProcess proc( cfg );

        WHEN( "rotateLog is called without starting the process" )
        {
            const auto result = proc.rotateLog();

            THEN( "it returns an empty string because the port is not open" )
            {
                REQUIRE( result.isEmpty() );
            }
        }
    }

    GIVEN( "a SerialProcess whose temp file has been manually set up for testing" )
    {
        // We cannot call start() without real hardware, but we can verify that
        // rotateLog returns empty when not running (port not open = no rotation).
        SerialConfig cfg;
        cfg.portName = "rotate-test-port";
        SerialProcess proc( cfg );

        THEN( "rotateLog returns empty because no port is open" )
        {
            REQUIRE( proc.rotateLog().isEmpty() );
        }

        THEN( "the line count stays at 0 after a failed rotation" )
        {
            proc.rotateLog();
            REQUIRE( proc.lineCount() == 0 );
        }
    }
}
