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
 * @file generatelogpath_test.cpp
 * @brief BDD tests for SerialProcess::generateLogPath().
 *
 * Log file names only have one-second resolution, so two captures in the
 * same second (a quick rotation, or Stop and Start) must still get
 * distinct files; and the port name must be usable in a file name
 * on every platform.
 */

#include <catch2/catch.hpp>

#include "serialprocess.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using serial_monitor::SerialProcess;

namespace {

void touch( const QString& path )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
}

} // namespace

SCENARIO( "generateLogPath names a new log file", "[generatelogpath]" )
{
    const QDateTime timestamp( QDate( 2026, 9, 30 ), QTime( 14, 5, 9 ) );
    QTemporaryDir dir;

    GIVEN( "an empty log directory" )
    {
        WHEN( "generating a path for a port" )
        {
            const auto path = SerialProcess::generateLogPath( dir.path(), "ttyUSB0", timestamp );

            THEN( "it is <date>_<time>_<port>.log inside the directory" )
            {
                REQUIRE( path == dir.filePath( "2026-09-30_140509_ttyUSB0.log" ) );
            }
        }

        WHEN( "generating a path for a port given as a device path" )
        {
            const auto path
                = SerialProcess::generateLogPath( dir.path(), "/dev/tty.usbserial-A1", timestamp );

            THEN( "characters that are invalid in file names are replaced" )
            {
                REQUIRE( path == dir.filePath( "2026-09-30_140509__dev_tty.usbserial-A1.log" ) );
            }
        }
    }

    GIVEN( "a log file with that name already exists" )
    {
        const auto taken = dir.filePath( "2026-09-30_140509_ttyUSB0.log" );
        touch( taken );

        WHEN( "generating a path for the same port and second" )
        {
            const auto path = SerialProcess::generateLogPath( dir.path(), "ttyUSB0", timestamp );

            THEN( "a numbered name that does not exist yet is returned" )
            {
                REQUIRE( path == dir.filePath( "2026-09-30_140509_ttyUSB0_2.log" ) );
                REQUIRE_FALSE( QFileInfo::exists( path ) );
            }
        }

        AND_GIVEN( "the first numbered name is taken as well" )
        {
            touch( dir.filePath( "2026-09-30_140509_ttyUSB0_2.log" ) );

            THEN( "the next number is used" )
            {
                REQUIRE( SerialProcess::generateLogPath( dir.path(), "ttyUSB0", timestamp )
                         == dir.filePath( "2026-09-30_140509_ttyUSB0_3.log" ) );
            }
        }
    }
}
