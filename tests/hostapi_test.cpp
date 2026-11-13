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
 * @file hostapi_test.cpp
 * @brief BDD tests for CApiHost, the Host of the C ABI, and Host::warn().
 *
 * CApiHost forwards to a FakeHost's C table.  The host decodes every string
 * it receives with QString::fromUtf8(), so the adapter must hand it UTF-8
 * regardless of the local 8-bit codec.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "host.h"

#include <QDir>

#include <stdexcept>

using serial_monitor::CApiHost;
using serial_test::FakeHost;

SCENARIO( "the C ABI's host forwards to the host's table", "[hostapi]" )
{
    GIVEN( "a host table that decodes every string as UTF-8" )
    {
        FakeHost fake;
        CApiHost host( fake.api(), fake.handle(), fake.tempRoot() );
        const auto text = QString::fromUtf8( "Gerät \xE2\x86\x92 COM3 \xE2\x9C\x93" );

        WHEN( "it logs a message with non-ASCII characters" )
        {
            host.log( LOGSQUIRL_LOG_INFO, text );

            THEN( "the host receives it unchanged, at its level" )
            {
                REQUIRE( fake.logs == QStringList{ text } );
                REQUIRE( fake.logLevels == QList<int>{ LOGSQUIRL_LOG_INFO } );
                REQUIRE( fake.notifications.isEmpty() );
            }
        }

        WHEN( "it shows a notification with non-ASCII characters" )
        {
            host.notify( text );

            THEN( "the host receives it unchanged" )
            {
                REQUIRE( fake.notifications == QStringList{ text } );
                REQUIRE( fake.logs.isEmpty() );
            }
        }

        WHEN( "it warns" )
        {
            host.warn( text );

            THEN( "the host logs it once as a warning, and notifies it once" )
            {
                REQUIRE( fake.logs == QStringList{ text } );
                REQUIRE( fake.logLevels == QList<int>{ LOGSQUIRL_LOG_WARNING } );
                REQUIRE( fake.notifications == QStringList{ text } );
            }
        }

        WHEN( "it warns of an error" )
        {
            host.warn( text, LOGSQUIRL_LOG_ERROR );

            THEN( "the host logs it once as an error, and notifies it once" )
            {
                REQUIRE( fake.logLevels == QList<int>{ LOGSQUIRL_LOG_ERROR } );
                REQUIRE( fake.notifications == QStringList{ text } );
            }
        }

        WHEN( "it opens a file in a followed tab" )
        {
            const auto path = QDir( fake.tempRoot() ).filePath( "Gerät.log" );
            host.openFollowed( path );

            THEN( "the host opens it in follow mode" )
            {
                REQUIRE( fake.openedFiles == QStringList{ path } );
                REQUIRE( fake.followedFiles == QStringList{ path } );
            }
        }

        THEN( "its directories are the host's configuration directory and the given root" )
        {
            REQUIRE( host.configDir() == fake.configDir() );
            REQUIRE( host.tempRoot() == fake.tempRoot() );
        }

        AND_GIVEN( "a host without a configuration directory" )
        {
            fake.hasConfigDir = false;

            THEN( "the configuration directory is empty" )
            {
                REQUIRE( host.configDir().isEmpty() );
            }
        }
    }

    GIVEN( "a host table and no temporary root" )
    {
        FakeHost fake;
        const CApiHost host( fake.api(), fake.handle() );

        THEN( "the temporary root is the system's" )
        {
            REQUIRE( host.tempRoot() == QDir::tempPath() );
        }
    }

    GIVEN( "no host table (the plugin is not initialised)" )
    {
        CApiHost host;

        WHEN( "it logs, notifies, warns and opens a tab" )
        {
            host.log( LOGSQUIRL_LOG_INFO, "ignored" );
            host.notify( "ignored" );
            host.warn( "ignored" );
            host.openFollowed( "/ignored.log" );

            THEN( "nothing happens" )
            {
                REQUIRE_FALSE( host.isAttached() );
            }
        }

        THEN( "it still names directories to fall back to" )
        {
            REQUIRE_FALSE( host.configDir().isEmpty() );
            REQUIRE( host.tempRoot() == QDir::tempPath() );
        }
    }
}

SCENARIO( "guarded() logs an exception through the host it is given", "[hostapi]" )
{
    GIVEN( "a host" )
    {
        FakeHost host;

        WHEN( "work run through it throws" )
        {
            const bool ok = serial_monitor::guarded(
                host, "testing", [] { throw std::runtime_error( "it broke" ); } );

            THEN( "it returns false, and the host's log says what failed and why" )
            {
                REQUIRE_FALSE( ok );
                REQUIRE( host.logs
                         == QStringList{ "Serial Monitor plugin: testing failed: it broke" } );
                REQUIRE( host.logLevels == QList<int>{ LOGSQUIRL_LOG_ERROR } );
            }
        }

        WHEN( "the host's log throws as well" )
        {
            host.failLogContaining = "failed";
            bool ok = true;
            REQUIRE_NOTHROW( ok = serial_monitor::guarded( host, "testing", [] {
                                 throw std::runtime_error( "it broke" );
                             } ) );

            THEN( "nothing escapes" )
            {
                REQUIRE_FALSE( ok );
            }
        }
    }
}
