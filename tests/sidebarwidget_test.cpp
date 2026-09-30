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
 * @file sidebarwidget_test.cpp
 * @brief BDD tests for the Serial sidebar panel.
 *
 * The panel lists the system's serial ports, so a pseudo-terminal used as
 * the device (see pseudoterminal.h) is never the selected port.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "portwidget.h"
#include "pseudoterminal.h"
#include "sidebarwidget.h"

#include <QLineEdit>
#include <QPushButton>

using serial_monitor::PortWidget;
using serial_monitor::SerialConfig;
using serial_monitor::SidebarWidget;
using serial_test::FakeHost;

#ifdef Q_OS_UNIX
SCENARIO( "a command is only sent to the selected port", "[sidebarwidget]" )
{
    GIVEN( "a capture on one port while the sidebar has another selected" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        PortWidget portWidget;
        SidebarWidget sidebar( &portWidget );

        SerialConfig cfg;
        cfg.portName = device.devicePath();
        REQUIRE( portWidget.startSession( cfg ) );
        host.notifications.clear();

        QLineEdit* sendEdit = nullptr;
        for ( auto* edit : sidebar.findChildren<QLineEdit*>() ) {
            if ( edit->placeholderText().startsWith( "Enter command" ) ) {
                sendEdit = edit;
            }
        }
        REQUIRE( sendEdit );

        WHEN( "the user sends a command" )
        {
            sendEdit->setText( "reboot" );
            Q_EMIT sendEdit->returnPressed();

            THEN( "it does not go to the other port's device" )
            {
                serial_test::processEventsFor( 200 ); // QSerialPort writes from the event loop
                REQUIRE( device.receive().isEmpty() );
            }

            THEN( "the user is told why nothing was sent, and the command is kept" )
            {
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( sendEdit->text() == "reboot" );
            }
        }

        portWidget.stopAll();
    }
}
#endif

SCENARIO( "the sidebar shares the dialog's port list", "[sidebarwidget]" )
{
    GIVEN( "a port widget that has scanned the ports" )
    {
        FakeHost host;
        PortWidget portWidget;
        const auto scansBefore = host.logs.filter( "Discovered" ).size();

        WHEN( "the sidebar is created" )
        {
            SidebarWidget sidebar( &portWidget );

            THEN( "it does not enumerate the ports again" )
            {
                REQUIRE( host.logs.filter( "Discovered" ).size() == scansBefore );
            }

            AND_WHEN( "the user clicks the sidebar's Refresh button" )
            {
                auto* refresh = sidebar.findChild<QPushButton*>( "refresh" );
                REQUIRE( refresh );
                refresh->click();

                THEN( "the ports are enumerated once, for both" )
                {
                    REQUIRE( host.logs.filter( "Discovered" ).size() == scansBefore + 1 );
                }
            }
        }
    }
}
