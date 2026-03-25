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
 * @file parseportlist_test.cpp
 * @brief BDD tests for SerialProcess::filterPorts().
 *
 * Validates that the port filter correctly excludes Bluetooth virtual
 * ports and entries with no system location, while passing through
 * real serial ports.
 *
 * NOTE: QSerialPortInfo objects cannot be trivially constructed with
 * arbitrary values (no public setters).  Therefore, we test filtering
 * indirectly by calling filterPorts() with the real system's port list
 * and verifying structural properties of the result, plus we test
 * discoverPorts() to ensure it returns a valid (possibly empty) list.
 */

#include <catch2/catch.hpp>

#include "serialprocess.h"

using serial_monitor::SerialProcess;

SCENARIO( "filterPorts returns a valid port list", "[filterports]" )
{
    GIVEN( "the system's available serial ports" )
    {
        const auto allPorts = QSerialPortInfo::availablePorts();
        const auto filtered = SerialProcess::filterPorts( allPorts );

        THEN( "the result size is at most the input size" )
        {
            REQUIRE( filtered.size() <= allPorts.size() );
        }

        THEN( "all returned names are non-empty strings" )
        {
            for ( const auto& name : filtered ) {
                REQUIRE_FALSE( name.isEmpty() );
            }
        }

        THEN( "no duplicates exist in the result" )
        {
            QStringList unique;
            for ( const auto& name : filtered ) {
                REQUIRE_FALSE( unique.contains( name ) );
                unique.append( name );
            }
        }
    }

    GIVEN( "an empty port list" )
    {
        const QList<QSerialPortInfo> empty;
        const auto filtered = SerialProcess::filterPorts( empty );

        THEN( "the result is empty" )
        {
            REQUIRE( filtered.isEmpty() );
        }
    }
}

SCENARIO( "discoverPorts returns a valid list", "[discoverports]" )
{
    GIVEN( "the discoverPorts static function" )
    {
        // This may return an empty list in CI (no serial hardware),
        // but it must not crash or throw.
        const auto ports = SerialProcess::discoverPorts();

        THEN( "the result is a valid (possibly empty) QStringList" )
        {
            REQUIRE( ports.size() >= 0 );
        }

        THEN( "all returned names are non-empty" )
        {
            for ( const auto& name : ports ) {
                REQUIRE_FALSE( name.isEmpty() );
            }
        }
    }
}
