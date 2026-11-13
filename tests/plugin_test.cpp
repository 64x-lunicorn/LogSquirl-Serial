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

#include <cstring>
#include <memory>

#include "connectiontile.h"
#include "fakehost.h"
#include "plugin.h"
#include "portdialog.h"
#include "profiles.h"
#include "pseudoterminal.h"
#include "readonlydir.h"
#include "sessions.h"
#include "sidebarwidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QWindow>

using serial_test::FakeHost;
using serial_test::waitFor;

extern "C" int logsquirl_plugin_init( const LogSquirlHostApi* api, void* handle );
extern "C" int logsquirl_plugin_init_ex( const LogSquirlHostApi* api, void* handle,
                                         size_t api_size );
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
                REQUIRE_FALSE(
                    serial_monitor::ProfileStore( host.configDir() ).defaults().timestamps );
            }

            THEN( "a new configuration in the dialog proposes it" )
            {
                serial_monitor::g_state.dialog->newConfiguration();
                auto* dialogTimestamps
                    = serial_monitor::g_state.dialog->findChild<QCheckBox*>( "timestamps" );
                REQUIRE( dialogTimestamps );
                REQUIRE_FALSE( dialogTimestamps->isChecked() );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "the Configure dialog sets the size at which log files are rotated", "[plugin]" )
{
    GIVEN( "an initialised plugin" )
    {
        FakeHost host;
        REQUIRE(
            logsquirl_plugin_init( serial_monitor::g_state.api, serial_monitor::g_state.handle )
            == 0 );

        WHEN( "the user opens Plugins → Configure" )
        {
            int shownMiB = -1;
            QString neverText;
            QTimer::singleShot( 0, [ &shownMiB, &neverText ]() {
                auto* dialog = qobject_cast<QDialog*>( QApplication::activeModalWidget() );
                if ( !dialog ) {
                    return;
                }
                auto* maxSize = dialog->findChild<QSpinBox*>( "maxFileMiB" );
                if ( !maxSize ) {
                    dialog->reject();
                    return;
                }
                shownMiB = maxSize->value();
                neverText = maxSize->specialValueText();
                maxSize->setValue( 50 );
                dialog->accept();
            } );
            logsquirl_plugin_configure( nullptr );

            THEN( "it shows the default size, offers no limit, and saves the new size" )
            {
                REQUIRE( shownMiB == serial_monitor::kDefaultMaxFileMiB );
                REQUIRE_FALSE( neverText.isEmpty() );
                REQUIRE( serial_monitor::ProfileStore( host.configDir() ).defaults().maxFileMiB
                         == 50 );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "the Configure dialog never saves where it should not", "[plugin]" )
{
    GIVEN( "an initialised plugin in a host without a configuration directory" )
    {
        FakeHost host;
        host.hasConfigDir = false;
        REQUIRE(
            logsquirl_plugin_init( serial_monitor::g_state.api, serial_monitor::g_state.handle )
            == 0 );
        host.notifications.clear();

        WHEN( "the user opens Plugins → Configure" )
        {
            auto dialogShown = std::make_shared<bool>( false );
            QTimer::singleShot( 0, [ dialogShown ]() {
                if ( auto* dialog = qobject_cast<QDialog*>( QApplication::activeModalWidget() ) ) {
                    *dialogShown = true;
                    dialog->accept();
                }
            } );
            logsquirl_plugin_configure( nullptr );
            serial_test::processEventsFor( 50 );

            THEN( "nothing can be saved, so no dialog is shown, and the user is told why" )
            {
                REQUIRE_FALSE( *dialogShown );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "no configuration directory" ) );
                REQUIRE( host.logs.filter( "no configuration directory" ).size() == 1 );
            }
        }

        logsquirl_plugin_shutdown();
    }

#ifdef Q_OS_UNIX
    GIVEN( "an initialised plugin whose configuration directory cannot be written" )
    {
        if ( !serial_test::ReadOnlyDir::isEnforced() ) {
            WARN( "File permissions are not enforced (running as root?); skipped." );
            return;
        }

        FakeHost host;
        REQUIRE(
            logsquirl_plugin_init( serial_monitor::g_state.api, serial_monitor::g_state.handle )
            == 0 );
        host.notifications.clear();

        WHEN( "the user saves new defaults in Plugins → Configure" )
        {
            QTimer::singleShot( 0, []() {
                if ( auto* dialog = qobject_cast<QDialog*>( QApplication::activeModalWidget() ) ) {
                    if ( auto* timestamps = dialog->findChild<QCheckBox*>() ) {
                        timestamps->setChecked( false );
                    }
                    dialog->accept();
                }
            } );
            {
                const serial_test::ReadOnlyDir readOnly( host.configDir() );
                logsquirl_plugin_configure( nullptr );
            }

            THEN( "the user is told that they were not saved" )
            {
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "could not be saved" ) );
                REQUIRE( host.logs.filter( "Default baud rate set" ).isEmpty() );
            }
        }

        logsquirl_plugin_shutdown();
    }
#endif
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
        auto* widget = serial_monitor::g_state.sessions;
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
                REQUIRE( serial_monitor::g_state.sessions->startSession(
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
        auto* widget = serial_monitor::g_state.sessions;
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

SCENARIO( "the plugin registers its sidebar tab for its lifetime", "[plugin]" )
{
    GIVEN( "a host" )
    {
        FakeHost host;

        WHEN( "the plugin is initialised" )
        {
            REQUIRE( logsquirl_plugin_init( host.api(), &host ) == 0 );

            THEN( "its sidebar widget is registered as a tab" )
            {
                REQUIRE( host.sidebarTabs.size() == 1 );
                REQUIRE( host.sidebarTabs.first() == serial_monitor::g_state.sidebarWidget );
            }

            AND_WHEN( "it is shut down" )
            {
                logsquirl_plugin_shutdown();

                THEN( "the tab is unregistered and the state cleared" )
                {
                    REQUIRE( host.sidebarTabs.isEmpty() );
                    REQUIRE( serial_monitor::g_state.sidebarWidget == nullptr );
                    REQUIRE( serial_monitor::g_state.dialog == nullptr );
                    REQUIRE( serial_monitor::g_state.profiles == nullptr );
                    REQUIRE( serial_monitor::g_state.sessions == nullptr );
                    REQUIRE_FALSE( serial_monitor::g_state.initialised );
                }
            }

            logsquirl_plugin_shutdown();
        }
    }
}

namespace {

/// The tab the plugin registered.
serial_monitor::SidebarWidget* sidebarTab( const FakeHost& host )
{
    REQUIRE( host.sidebarTabs.size() == 1 );
    return static_cast<serial_monitor::SidebarWidget*>( host.sidebarTabs.first() );
}

} // namespace

SCENARIO( "saved configurations survive a restart of LogSquirl", "[plugin]" )
{
    GIVEN( "a configuration saved in the dialog of an initialised plugin" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), &host ) == 0 );
        auto* dialog = serial_monitor::g_state.dialog;
        dialog->findChild<QComboBox*>( "profile" )->setEditText( "board" );
        dialog->findChild<QComboBox*>( "port" )->setEditText( "/dev/ttyUSB0" );
        dialog->findChild<QPushButton*>( "saveProfile" )->click();
        REQUIRE( sidebarTab( host )->tile( "board" ) );

        WHEN( "the plugin is shut down and started again, as when LogSquirl restarts" )
        {
            logsquirl_plugin_shutdown();
            REQUIRE( logsquirl_plugin_init( host.api(), &host ) == 0 );

            THEN( "the sidebar shows its tile again" )
            {
                const auto* tile = sidebarTab( host )->tile( "board" );
                REQUIRE( tile );
                REQUIRE(
                    tile->findChild<QLabel*>( "summary" )->text().startsWith( "/dev/ttyUSB0\n" ) );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "the sidebar opens the dialog", "[plugin]" )
{
    GIVEN( "an initialised plugin with a saved configuration" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), &host ) == 0 );
        serial_monitor::PortProfile profile;
        profile.name = "board";
        profile.serial.portName = "/dev/ttyUSB0";
        REQUIRE( serial_monitor::g_state.profiles->upsert( profile ).isEmpty() );
        auto* dialog = serial_monitor::g_state.dialog;

        WHEN( "the tile's Edit… is clicked" )
        {
            sidebarTab( host )->tile( "board" )->findChild<QPushButton*>( "edit" )->click();

            THEN( "the dialog opens with that configuration" )
            {
                REQUIRE( dialog->isVisible() );
                REQUIRE( dialog->findChild<QComboBox*>( "profile" )->currentText() == "board" );
                REQUIRE( dialog->findChild<QComboBox*>( "port" )->currentText() == "/dev/ttyUSB0" );
            }
        }

        WHEN( "New configuration… is clicked" )
        {
            sidebarTab( host )->findChild<QPushButton*>( "newConfiguration" )->click();

            THEN( "the dialog opens empty" )
            {
                REQUIRE( dialog->isVisible() );
                REQUIRE( dialog->findChild<QComboBox*>( "profile" )->currentText().isEmpty() );
                REQUIRE( dialog->findChild<QComboBox*>( "port" )->currentText().isEmpty() );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

#ifdef Q_OS_UNIX
SCENARIO( "a tile's capture stops when the plugin shuts down", "[plugin]" )
{
    GIVEN( "an initialised plugin with a capturing tile" )
    {
        FakeHost host;
        serial_test::PseudoTerminal device;
        REQUIRE( logsquirl_plugin_init( host.api(), &host ) == 0 );
        serial_monitor::PortProfile profile;
        profile.name = "board";
        profile.serial.portName = device.devicePath();
        profile.logDir = host.tempRoot() + "/logs";
        REQUIRE( serial_monitor::g_state.profiles->upsert( profile ).isEmpty() );
        sidebarTab( host )->tile( "board" )->findChild<QPushButton*>( "startStop" )->click();
        REQUIRE( serial_monitor::g_state.sessions->portOf( "board" ) == device.devicePath() );
        REQUIRE( host.followedFiles.size() == 1 );

        WHEN( "the plugin is shut down" )
        {
            logsquirl_plugin_shutdown();

            THEN( "the capture's file stays for its tab" )
            {
                REQUIRE( QFileInfo::exists( host.followedFiles.first() ) );
            }
        }

        logsquirl_plugin_shutdown();
    }
}
#endif

// MSVC's default /EHsc assumes that functions of C linkage, like the host
// API's, never throw, and may drop the handlers this test relies on.
#ifndef _MSC_VER
SCENARIO( "no exception leaves an entry point", "[plugin]" )
{
    GIVEN( "a host whose register_sidebar_tab() throws" )
    {
        FakeHost host;
        host.failSidebarTab = true;

        WHEN( "the plugin is initialised" )
        {
            int rc = 0;
            REQUIRE_NOTHROW( rc = logsquirl_plugin_init( host.api(), &host ) );

            THEN( "initialisation fails, is logged, and leaves nothing behind" )
            {
                REQUIRE( rc != 0 );
                REQUIRE( serial_monitor::g_state.sidebarWidget == nullptr );
                REQUIRE( serial_monitor::g_state.dialog == nullptr );
                REQUIRE( serial_monitor::g_state.sessions == nullptr );
                REQUIRE_FALSE( host.logs.filter( "no room for another tab" ).isEmpty() );
            }
        }

        // The host would not call shutdown() after a failed init, but it must be harmless.
        REQUIRE_NOTHROW( logsquirl_plugin_shutdown() );
    }

    GIVEN( "a host that fails after the sidebar tab was registered" )
    {
        FakeHost host;
        host.failLogContaining = "ready";

        WHEN( "the plugin is initialised" )
        {
            int rc = 0;
            REQUIRE_NOTHROW( rc = logsquirl_plugin_init( host.api(), &host ) );

            THEN( "initialisation fails and the tab is unregistered before its widget goes" )
            {
                REQUIRE( rc != 0 );
                REQUIRE( host.sidebarTabs.isEmpty() );
                REQUIRE( serial_monitor::g_state.sidebarWidget == nullptr );
                REQUIRE( serial_monitor::g_state.dialog == nullptr );
                REQUIRE( serial_monitor::g_state.sessions == nullptr );
            }
        }

        REQUIRE_NOTHROW( logsquirl_plugin_shutdown() );
    }

    GIVEN( "an initialised plugin in a host whose log throws on shutdown" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), &host ) == 0 );
        host.failLogContaining = "shutting down";

        WHEN( "it is shut down" )
        {
            REQUIRE_NOTHROW( logsquirl_plugin_shutdown() );

            THEN( "every other step still ran" )
            {
                REQUIRE( host.sidebarTabs.isEmpty() );
                REQUIRE( serial_monitor::g_state.sidebarWidget == nullptr );
                REQUIRE( serial_monitor::g_state.dialog == nullptr );
                REQUIRE( serial_monitor::g_state.sessions == nullptr );
                REQUIRE_FALSE( serial_monitor::g_state.initialised );
            }
        }
    }
}
#endif

SCENARIO( "the plugin learns which functions the host has from the size of its table", "[plugin]" )
{
    GIVEN( "a host of LogSquirl 26.11 or later, which calls init_ex() with its table's size" )
    {
        FakeHost host;

        WHEN( "the plugin is initialised" )
        {
            REQUIRE( logsquirl_plugin_init_ex( host.api(), &host, sizeof( LogSquirlHostApi ) )
                     == 0 );

            THEN( "it keeps the size, and every later function counts as there" )
            {
                REQUIRE( serial_monitor::g_state.hostApiSize == sizeof( LogSquirlHostApi ) );
                REQUIRE(
                    LOGSQUIRL_HOST_API_HAS( serial_monitor::g_state.hostApiSize, open_regex_lab ) );
            }

            AND_WHEN( "it is shut down" )
            {
                logsquirl_plugin_shutdown();

                THEN( "the size is forgotten" )
                {
                    REQUIRE( serial_monitor::g_state.hostApiSize == 0 );
                }
            }

            logsquirl_plugin_shutdown();
        }
    }

    GIVEN( "a host of LogSquirl 26.10, whose table ends before the later functions" )
    {
        FakeHost host;
        // Exactly the base table in a block of its own: a read past it is a
        // real out-of-bounds read, which an AddressSanitizer build reports.
        const auto block = std::make_unique<unsigned char[]>( LOGSQUIRL_HOST_API_BASE_SIZE );
        std::memcpy( block.get(), host.api(), LOGSQUIRL_HOST_API_BASE_SIZE );
        const auto* baseTable = reinterpret_cast<const LogSquirlHostApi*>( block.get() );

        WHEN( "it calls init()" )
        {
            REQUIRE( logsquirl_plugin_init( baseTable, &host ) == 0 );

            THEN( "the plugin starts with the base table's size and no later function" )
            {
                REQUIRE( serial_monitor::g_state.hostApiSize == LOGSQUIRL_HOST_API_BASE_SIZE );
                REQUIRE_FALSE(
                    LOGSQUIRL_HOST_API_HAS( serial_monitor::g_state.hostApiSize, open_regex_lab ) );
                REQUIRE( host.sidebarTabs.size() == 1 );
            }

            logsquirl_plugin_shutdown();
        }
    }
}
