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
 * @brief BDD tests for the session bookkeeping in PortWidget.
 *
 * PortWidget owns the running SerialProcess sessions for both the dialog
 * and the sidebar.  These tests drive it through its public API with a
 * FakeHost and, on Unix, a pseudo-terminal as the device (see
 * pseudoterminal.h).
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "portwidget.h"
#include "pseudoterminal.h"

#include <QDir>
#include <QTemporaryDir>

using serial_monitor::PortWidget;
using serial_monitor::SerialConfig;
using serial_test::FakeHost;
using serial_test::waitFor;

namespace {

SerialConfig configFor( const QString& portName )
{
    SerialConfig cfg;
    cfg.portName = portName;
    return cfg;
}

} // namespace

SCENARIO( "a session is only accepted when its port opens", "[portwidget]" )
{
    GIVEN( "a port that does not exist" )
    {
        FakeHost host;
        PortWidget widget;
        QTemporaryDir logDir;
        const QString port = "logsquirl-test-no-such-port";

        WHEN( "starting a session with a save path" )
        {
            const auto started
                = widget.startSession( configFor( port ), logDir.filePath( "capture.log" ) );

            THEN( "the session is rejected" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( widget.activeSessionCount() == 0 );
                REQUIRE_FALSE( widget.isSessionActive( port ) );
            }

            THEN( "no tab is opened and no empty file is left behind" )
            {
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE( QDir( logDir.path() ).isEmpty() );
            }

            THEN( "the user is told once why" )
            {
                REQUIRE( host.notifications.size() == 1 );
            }
        }
    }

#ifdef Q_OS_UNIX
    GIVEN( "a device on a working port" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        PortWidget widget;
        QTemporaryDir logDir;

        WHEN( "starting a session" )
        {
            const auto started = widget.startSession( configFor( device.devicePath() ),
                                                      logDir.filePath( "capture.log" ) );

            THEN( "the session is accepted and its file opened in a tab" )
            {
                INFO( host.notifications.join( '\n' ).toStdString() );
                REQUIRE( started );
                REQUIRE( widget.isSessionActive( device.devicePath() ) );
                REQUIRE( host.openedFiles.size() == 1 );
            }

            widget.stopAll();
        }
    }
#endif
}
