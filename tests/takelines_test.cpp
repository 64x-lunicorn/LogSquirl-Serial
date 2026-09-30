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
 * @file takelines_test.cpp
 * @brief BDD tests for SerialProcess::takeLines().
 *
 * takeLines() turns the bytes read from a serial port into complete lines.
 * Devices end lines with "\n", "\r\n" or a lone "\r", and some never end
 * them at all (binary data, or a progress display that redraws with "\r"),
 * so the read buffer must not grow without bound.
 */

#include <catch2/catch.hpp>

#include "serialprocess.h"

using serial_monitor::SerialProcess;

using Lines = QList<QByteArray>;

SCENARIO( "takeLines splits complete lines off the read buffer", "[takelines]" )
{
    GIVEN( "a buffer with LF-terminated lines and a partial last line" )
    {
        QByteArray buffer = "first\nsecond\nthird";

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer );

            THEN( "the complete lines are returned without their terminator" )
            {
                REQUIRE( lines == Lines{ "first", "second" } );
            }

            THEN( "the partial line stays in the buffer" )
            {
                REQUIRE( buffer == "third" );
            }
        }
    }

    GIVEN( "a buffer with CRLF-terminated lines" )
    {
        QByteArray buffer = "first\r\nsecond\r\n";

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer );

            THEN( "each CRLF ends exactly one line" )
            {
                REQUIRE( lines == Lines{ "first", "second" } );
                REQUIRE( buffer.isEmpty() );
            }
        }
    }

    GIVEN( "a buffer with CR-only line endings" )
    {
        QByteArray buffer = "first\rsecond\rthird";

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer );

            THEN( "a lone CR ends a line too" )
            {
                REQUIRE( lines == Lines{ "first", "second" } );
                REQUIRE( buffer == "third" );
            }
        }
    }

    GIVEN( "a CRLF terminator split across two reads" )
    {
        QByteArray buffer = "first\r";

        WHEN( "taking lines before and after the LF arrives" )
        {
            const auto before = SerialProcess::takeLines( buffer );
            buffer.append( "\nsecond" );
            const auto after = SerialProcess::takeLines( buffer );

            THEN( "the CR waits for the next byte and no empty line appears" )
            {
                REQUIRE( before.isEmpty() );
                REQUIRE( after == Lines{ "first" } );
                REQUIRE( buffer == "second" );
            }
        }
    }

    GIVEN( "a CR at the end of a read followed by a new line's data" )
    {
        QByteArray buffer = "first\r";

        WHEN( "the next read does not start with LF" )
        {
            SerialProcess::takeLines( buffer );
            buffer.append( "second\r\n" );
            const auto lines = SerialProcess::takeLines( buffer );

            THEN( "the CR ended the first line" )
            {
                REQUIRE( lines == Lines{ "first", "second" } );
                REQUIRE( buffer.isEmpty() );
            }
        }
    }

    GIVEN( "a buffer with empty lines in every line ending style" )
    {
        QByteArray buffer = "\n\r\n\r\r\n";

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer );

            THEN( "each empty line is kept" )
            {
                REQUIRE( lines == Lines{ "", "", "", "" } );
            }
        }
    }

    GIVEN( "a buffer without any line terminator" )
    {
        QByteArray buffer = "partial";

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer );

            THEN( "nothing is taken" )
            {
                REQUIRE( lines.isEmpty() );
                REQUIRE( buffer == "partial" );
            }
        }
    }
}

SCENARIO( "takeLines caps an unterminated line", "[takelines]" )
{
    GIVEN( "more unterminated data than the maximum line length" )
    {
        QByteArray buffer = "done\n0123456789";

        WHEN( "taking lines with a maximum line length of 8" )
        {
            const auto lines = SerialProcess::takeLines( buffer, 8 );

            THEN( "the unterminated data is forced out as a line of its own" )
            {
                REQUIRE( lines == Lines{ "done", "0123456789" } );
                REQUIRE( buffer.isEmpty() );
            }
        }
    }

    GIVEN( "unterminated data within the maximum line length" )
    {
        QByteArray buffer = "01234567";

        WHEN( "taking lines with a maximum line length of 8" )
        {
            const auto lines = SerialProcess::takeLines( buffer, 8 );

            THEN( "the data keeps waiting for its terminator" )
            {
                REQUIRE( lines.isEmpty() );
                REQUIRE( buffer == "01234567" );
            }
        }
    }

    GIVEN( "a binary stream without line endings" )
    {
        QByteArray buffer( SerialProcess::kMaxLineLength + 1, '\x01' );

        WHEN( "taking lines with the default maximum" )
        {
            const auto lines = SerialProcess::takeLines( buffer );

            THEN( "the buffer is emptied instead of growing" )
            {
                REQUIRE( lines.size() == 1 );
                REQUIRE( lines.first().size() == SerialProcess::kMaxLineLength + 1 );
                REQUIRE( buffer.isEmpty() );
            }
        }
    }
}
