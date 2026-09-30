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
 * @file plugin_test.cpp
 * @brief BDD tests for the plugin's lifecycle through its C entry points.
 *
 * Drives logsquirl_plugin_init() / logsquirl_plugin_shutdown() against a
 * FakeHost, and clicks the menu entry the plugin registers.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "plugin.h"
#include "portwidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <QWindow>

using serial_test::FakeHost;
using serial_test::waitFor;

extern "C" int logsquirl_plugin_init( const LogSquirlHostApi* api, void* handle );
extern "C" void logsquirl_plugin_shutdown( void );
extern "C" void logsquirl_plugin_configure( void* parent_widget );

SCENARIO( "the Serial Monitor dialog belongs to LogSquirl's window", "[plugin]" )
{
    GIVEN( "an initialised plugin and an active main window" )
    {
        FakeHost host;
        REQUIRE(
            logsquirl_plugin_init( serial_monitor::g_state.api, serial_monitor::g_state.handle )
            == 0 );
        REQUIRE( host.menuActions.size() == 1 );

        QWidget mainWindow;
        mainWindow.show();
        mainWindow.activateWindow();
        REQUIRE(
            waitFor( [ &mainWindow ]() { return QApplication::activeWindow() == &mainWindow; } ) );

        WHEN( "the user opens the dialog from the Plugins menu" )
        {
            host.menuActions.first().trigger();
            auto* dialog = serial_monitor::g_state.dialog;

            THEN( "it is shown on top of the main window" )
            {
                REQUIRE( dialog->isVisible() );
                REQUIRE( dialog->windowHandle()->transientParent() == mainWindow.windowHandle() );
            }

            THEN( "an open dialog does not keep LogSquirl running when the main window closes" )
            {
                REQUIRE_FALSE( dialog->testAttribute( Qt::WA_QuitOnClose ) );
            }

            THEN( "the plugin, not the window, owns it" )
            {
                REQUIRE( dialog->parent() == nullptr );
            }
        }

        logsquirl_plugin_shutdown();
        REQUIRE( serial_monitor::g_state.dialog == nullptr );
    }
}

SCENARIO( "the Configure dialog edits the default settings", "[plugin]" )
{
    GIVEN( "an initialised plugin with timestamps on by default" )
    {
        FakeHost host;
        REQUIRE(
            logsquirl_plugin_init( serial_monitor::g_state.api, serial_monitor::g_state.handle )
            == 0 );

        WHEN( "the user turns timestamps off in Plugins → Configure" )
        {
            bool foundCheckBox = false;
            QTimer::singleShot( 0, [ &foundCheckBox ]() {
                auto* dialog = qobject_cast<QDialog*>( QApplication::activeModalWidget() );
                if ( !dialog ) {
                    return;
                }
                if ( auto* timestamps = dialog->findChild<QCheckBox*>() ) {
                    foundCheckBox = true;
                    timestamps->setChecked( false );
                    dialog->accept();
                }
                else {
                    dialog->reject();
                }
            } );
            logsquirl_plugin_configure( nullptr );

            THEN( "the dialog offers the setting and saves it" )
            {
                REQUIRE( foundCheckBox );
                QSettings settings( host.configDir() + "/serial.ini", QSettings::IniFormat );
                REQUIRE_FALSE( settings.value( "serial/timestamps", true ).toBool() );
            }

            THEN( "the open Serial Monitor dialog takes it over" )
            {
                auto* dialogTimestamps
                    = serial_monitor::g_state.dialog->findChild<QCheckBox*>( "timestamps" );
                REQUIRE( dialogTimestamps );
                REQUIRE_FALSE( dialogTimestamps->isChecked() );
            }
        }

        logsquirl_plugin_shutdown();
    }
}
