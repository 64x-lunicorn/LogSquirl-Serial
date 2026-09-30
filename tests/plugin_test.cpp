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
#include "pseudoterminal.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
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

#ifdef Q_OS_UNIX
namespace {

/** Directory of the temporary file a session opened in a tab. */
QString dirOf( const QString& file )
{
    return QFileInfo( file ).absolutePath();
}

serial_monitor::SerialConfig configFor( const QString& portName )
{
    serial_monitor::SerialConfig cfg;
    cfg.portName = portName;
    return cfg;
}

} // namespace

SCENARIO( "temporary files are removed only when LogSquirl quits", "[plugin]" )
{
    GIVEN( "an initialised plugin with a stopped and a running, rotated temp-file session" )
    {
        FakeHost host;
        serial_test::PseudoTerminal firstDevice;
        serial_test::PseudoTerminal secondDevice;
        const auto* api = serial_monitor::g_state.api;
        auto* handle = serial_monitor::g_state.handle;
        REQUIRE( logsquirl_plugin_init( api, handle ) == 0 );
        auto* widget = serial_monitor::g_state.dialog;
        REQUIRE( widget->startSession( configFor( firstDevice.devicePath() ) ) );
        widget->stopSession( firstDevice.devicePath() );
        REQUIRE( widget->startSession( configFor( secondDevice.devicePath() ) ) );
        widget->rotateSession( secondDevice.devicePath() );
        REQUIRE( host.openedFiles.size() == 3 );
        const auto stoppedDir = dirOf( host.openedFiles.first() );
        const auto runningDir = dirOf( host.openedFiles.last() );
        REQUIRE( dirOf( host.openedFiles.at( 1 ) ) == runningDir );
        // Named after the process, in the temporary root
        const auto prefix
            = QString( "logsquirl-serial-%1-" ).arg( QCoreApplication::applicationPid() );
        for ( const auto& dir : { stoppedDir, runningDir } ) {
            REQUIRE( QFileInfo( dir ).fileName().startsWith( prefix ) );
            REQUIRE( QFileInfo( dirOf( dir ) ).canonicalFilePath()
                     == QFileInfo( host.tempRoot() ).canonicalFilePath() );
        }

        WHEN( "LogSquirl quits, which shuts the plugin down" )
        {
            QMetaObject::invokeMethod( QCoreApplication::instance(), "aboutToQuit" );
            logsquirl_plugin_shutdown();

            THEN( "the temporary directories of both sessions are removed" )
            {
                REQUIRE_FALSE( QFileInfo::exists( stoppedDir ) );
                REQUIRE_FALSE( QFileInfo::exists( runningDir ) );
            }

            AND_WHEN( "the plugin is loaded again and later disabled" )
            {
                host.openedFiles.clear();
                REQUIRE( logsquirl_plugin_init( api, handle ) == 0 );
                REQUIRE( serial_monitor::g_state.dialog->startSession(
                    configFor( firstDevice.devicePath() ) ) );
                const auto newDir = dirOf( host.openedFiles.first() );
                logsquirl_plugin_shutdown();

                THEN( "the earlier quit does not make it remove the new tab's file" )
                {
                    REQUIRE( QFileInfo::exists( newDir ) );
                }

                QDir( newDir ).removeRecursively();
            }
        }

        WHEN( "the plugin is disabled or updated while LogSquirl keeps running" )
        {
            logsquirl_plugin_shutdown();

            THEN( "the files of both sessions are kept for their open tabs" )
            {
                REQUIRE( QFileInfo::exists( host.openedFiles.first() ) );
                REQUIRE( QFileInfo::exists( host.openedFiles.last() ) );
            }

            AND_WHEN( "it is enabled again, and LogSquirl quits later" )
            {
                REQUIRE( logsquirl_plugin_init( api, handle ) == 0 );
                QMetaObject::invokeMethod( QCoreApplication::instance(), "aboutToQuit" );
                logsquirl_plugin_shutdown();

                THEN( "the new instance removes the files the earlier one left for its tabs" )
                {
                    REQUIRE_FALSE( QFileInfo::exists( stoppedDir ) );
                    REQUIRE_FALSE( QFileInfo::exists( runningDir ) );
                    REQUIRE( QDir( host.tempRoot() ).isEmpty() );
                }
            }
        }
    }
}

SCENARIO( "save paths are never removed", "[plugin]" )
{
    GIVEN( "a stopped and a rotated session that write to save paths below the temporary root" )
    {
        FakeHost host;
        serial_test::PseudoTerminal firstDevice;
        serial_test::PseudoTerminal secondDevice;
        REQUIRE(
            logsquirl_plugin_init( serial_monitor::g_state.api, serial_monitor::g_state.handle )
            == 0 );
        auto* widget = serial_monitor::g_state.dialog;
        const auto logDir = host.tempRoot() + "/logs";
        REQUIRE( widget->startSession( configFor( firstDevice.devicePath() ),
                                       logDir + "/stopped.log" ) );
        widget->stopSession( firstDevice.devicePath() );
        REQUIRE( widget->startSession( configFor( secondDevice.devicePath() ),
                                       logDir + "/rotated.log" ) );
        widget->rotateSession( secondDevice.devicePath() );
        const auto files = QDir( logDir ).entryList( QDir::Files );
        REQUIRE( files.size() == 3 );

        WHEN( "LogSquirl quits, which shuts the plugin down" )
        {
            QMetaObject::invokeMethod( QCoreApplication::instance(), "aboutToQuit" );
            logsquirl_plugin_shutdown();

            THEN( "every file in the log directory is kept" )
            {
                REQUIRE( QDir( logDir ).entryList( QDir::Files ) == files );
            }
        }
    }
}
#endif

SCENARIO( "temporary directories of LogSquirl processes that ended are swept", "[plugin]" )
{
    GIVEN( "directories left by a process that ended, and by one that runs" )
    {
        FakeHost host;
        const QDir root( host.tempRoot() );
        // No process has this ID: PIDs stay far below it on Linux and macOS,
        // and on Windows it is a multiple of 4 no process gets in practice.
        const QString dead = "logsquirl-serial-2147483644-AbC123";
#ifdef Q_OS_WIN
        const QString alive = "logsquirl-serial-4-AbC123"; // the System process
#else
        const QString alive = "logsquirl-serial-1-AbC123"; // init / launchd
#endif
        const QString unrelated = "logsquirl-serial-notapid";
        for ( const auto& name : { dead, alive, unrelated } ) {
            REQUIRE( root.mkpath( name + "/sub" ) );
            QFile file( root.filePath( name + "/sub/serial.log" ) );
            REQUIRE( file.open( QIODevice::WriteOnly ) );
        }

        WHEN( "the plugin is initialised" )
        {
            REQUIRE(
                logsquirl_plugin_init( serial_monitor::g_state.api, serial_monitor::g_state.handle )
                == 0 );
            logsquirl_plugin_shutdown();

            THEN( "only the ended process's directory is removed" )
            {
                REQUIRE_FALSE( root.exists( dead ) );
                REQUIRE( root.exists( alive + "/sub/serial.log" ) );
                REQUIRE( root.exists( unrelated + "/sub/serial.log" ) );
            }
        }

        WHEN( "the plugin shuts down as LogSquirl quits" )
        {
            REQUIRE(
                logsquirl_plugin_init( serial_monitor::g_state.api, serial_monitor::g_state.handle )
                == 0 );
            QMetaObject::invokeMethod( QCoreApplication::instance(), "aboutToQuit" );
            logsquirl_plugin_shutdown();

            THEN( "another running process's directory is kept" )
            {
                REQUIRE( root.exists( alive + "/sub/serial.log" ) );
                REQUIRE( root.exists( unrelated + "/sub/serial.log" ) );
            }
        }
    }

#ifdef Q_OS_UNIX
    GIVEN( "a link named like an ended process's directory, to a directory elsewhere" )
    {
        FakeHost host;
        QTemporaryDir target;
        REQUIRE( target.isValid() );
        QFile file( target.filePath( "keep.log" ) );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        file.close();
        const QDir root( host.tempRoot() );
        const QString link = "logsquirl-serial-2147483644-AbC123";
        REQUIRE( QFile::link( target.path(), root.filePath( link ) ) );

        WHEN( "the plugin is initialised" )
        {
            REQUIRE(
                logsquirl_plugin_init( serial_monitor::g_state.api, serial_monitor::g_state.handle )
                == 0 );
            logsquirl_plugin_shutdown();

            THEN( "the link is not followed" )
            {
                REQUIRE( QFileInfo::exists( target.filePath( "keep.log" ) ) );
            }
        }
    }
#endif
}
