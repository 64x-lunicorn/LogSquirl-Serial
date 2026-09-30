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
 *
 * A CR ends its line at once, even when it is the last byte read: a
 * device that ends lines with a lone CR must not see each line appear one
 * line late.  afterCr remembers it, so that an LF at the start of the
 * next read is taken as the second half of a CRLF pair.
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
        bool afterCr = false;

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer, afterCr );

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
        bool afterCr = false;

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer, afterCr );

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
        bool afterCr = false;

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer, afterCr );

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
        bool afterCr = false;

        WHEN( "taking lines before and after the LF arrives" )
        {
            const auto before = SerialProcess::takeLines( buffer, afterCr );
            buffer.append( "\nsecond" );
            const auto after = SerialProcess::takeLines( buffer, afterCr );

            THEN( "the CR ends the line at once, and the LF adds no empty line" )
            {
                REQUIRE( before == Lines{ "first" } );
                REQUIRE( after.isEmpty() );
                REQUIRE( buffer == "second" );
            }
        }
    }

    GIVEN( "a device that ends its lines with a lone CR, then goes quiet" )
    {
        QByteArray buffer = "first\rsecond\r";
        bool afterCr = false;

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer, afterCr );

            THEN( "the last line is not held back waiting for more data" )
            {
                REQUIRE( lines == Lines{ "first", "second" } );
                REQUIRE( buffer.isEmpty() );
            }
        }
    }

    GIVEN( "a CR at the end of a read followed by a new line's data" )
    {
        QByteArray buffer = "first\r";
        bool afterCr = false;

        WHEN( "the next read does not start with LF" )
        {
            const auto before = SerialProcess::takeLines( buffer, afterCr );
            buffer.append( "second\r\n" );
            const auto after = SerialProcess::takeLines( buffer, afterCr );

            THEN( "the CR ended the first line, and the second line is complete" )
            {
                REQUIRE( before == Lines{ "first" } );
                REQUIRE( after == Lines{ "second" } );
                REQUIRE( buffer.isEmpty() );
            }
        }
    }

    GIVEN( "a CR at the end of a read followed by an empty line" )
    {
        QByteArray buffer = "first\r";
        bool afterCr = false;

        WHEN( "the next read brings a CRLF pair and then another LF" )
        {
            SerialProcess::takeLines( buffer, afterCr );
            buffer.append( "\n\n" );
            const auto lines = SerialProcess::takeLines( buffer, afterCr );

            THEN( "only the LF that completes the CRLF pair is skipped" )
            {
                REQUIRE( lines == Lines{ "" } );
                REQUIRE( buffer.isEmpty() );
            }
        }
    }

    GIVEN( "a CR at the end of a read, and an empty read after it" )
    {
        QByteArray buffer = "first\r";
        bool afterCr = false;

        WHEN( "the LF arrives only in the read after the empty one" )
        {
            SerialProcess::takeLines( buffer, afterCr );
            SerialProcess::takeLines( buffer, afterCr );
            buffer.append( "\nsecond\n" );
            const auto lines = SerialProcess::takeLines( buffer, afterCr );

            THEN( "the LF still completes the CRLF pair" )
            {
                REQUIRE( lines == Lines{ "second" } );
            }
        }
    }

    GIVEN( "a buffer with empty lines in every line ending style" )
    {
        QByteArray buffer = "\n\r\n\r\r\n";
        bool afterCr = false;

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer, afterCr );

            THEN( "each empty line is kept" )
            {
                REQUIRE( lines == Lines{ "", "", "", "" } );
            }
        }
    }

    GIVEN( "a buffer without any line terminator" )
    {
        QByteArray buffer = "partial";
        bool afterCr = false;

        WHEN( "taking lines" )
        {
            const auto lines = SerialProcess::takeLines( buffer, afterCr );

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
        bool afterCr = false;

        WHEN( "taking lines with a maximum line length of 8" )
        {
            const auto lines = SerialProcess::takeLines( buffer, afterCr, 8 );

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
        bool afterCr = false;

        WHEN( "taking lines with a maximum line length of 8" )
        {
            const auto lines = SerialProcess::takeLines( buffer, afterCr, 8 );

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
        bool afterCr = false;

        WHEN( "taking lines with the default maximum" )
        {
            const auto lines = SerialProcess::takeLines( buffer, afterCr );

            THEN( "the buffer is emptied instead of growing" )
            {
                REQUIRE( lines.size() == 1 );
                REQUIRE( lines.first().size() == SerialProcess::kMaxLineLength + 1 );
                REQUIRE( buffer.isEmpty() );
            }
        }
    }
}
