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
 * @file portwidget_test.cpp
 * @brief BDD tests for the Serial Monitor dialog, a view of the Sessions.
 *
 * The sessions themselves are tested in sessions_test.cpp.  The dialog
 * lists the system's serial ports, so a pseudo-terminal used as the device
 * (see pseudoterminal.h) is never the selected port: these tests look at
 * what the dialog shows of all sessions.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "portwidget.h"
#include "pseudoterminal.h"
#include "sessions.h"

#include <QLabel>

using serial_monitor::PortWidget;
using serial_monitor::SerialConfig;
using serial_monitor::Sessions;
using serial_test::FakeHost;

namespace {

/// The dialog's status line, which counts the active sessions.
QString statusOf( const PortWidget& dialog )
{
    for ( const auto* label : dialog.findChildren<QLabel*>() ) {
        if ( label->text().contains( "session" ) ) {
            return label->text();
        }
    }
    return {};
}

} // namespace

SCENARIO( "the dialog shares the sessions' port list", "[portwidget]" )
{
    GIVEN( "sessions that have scanned the ports" )
    {
        FakeHost host;
        Sessions sessions;
        const auto scansBefore = host.logs.filter( "Discovered" ).size();

        WHEN( "the dialog is created" )
        {
            PortWidget dialog( &sessions );

            THEN( "it does not enumerate the ports again, and shows that nothing runs" )
            {
                REQUIRE( host.logs.filter( "Discovered" ).size() == scansBefore );
                REQUIRE( statusOf( dialog ) == "No active sessions" );
            }
        }
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "the dialog follows sessions it did not start", "[portwidget][signals]" )
{
    GIVEN( "a dialog over the sessions" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        Sessions sessions;
        PortWidget dialog( &sessions );
        SerialConfig cfg;
        cfg.portName = device.devicePath();

        WHEN( "a session is started elsewhere, as from the sidebar" )
        {
            REQUIRE( sessions.startSession( cfg ) );

            THEN( "the dialog counts it at once" )
            {
                REQUIRE( statusOf( dialog ) == "1 active session(s)" );
            }

            AND_WHEN( "it is stopped" )
            {
                sessions.stopSession( device.devicePath() );

                THEN( "the dialog no longer counts it" )
                {
                    REQUIRE( statusOf( dialog ) == "No active sessions" );
                }
            }

            sessions.stopAll();
        }
    }
}
#endif
