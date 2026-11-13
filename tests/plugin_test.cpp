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

using serial_test::waitFor;

namespace {

/// A FakeHost whose temporary root the plugin puts its temporary
/// directories in (setTempRootForTests()).
class FakeHost : public serial_test::FakeHost {
public:
    FakeHost()
    {
        serial_monitor::setTempRootForTests( tempRoot() );
    }

    ~FakeHost() override
    {
        serial_monitor::setTempRootForTests( {} );
    }
};

/// The widgets of type W there are now, wherever they are parented.
template <typename W>
QList<W*> widgetsOf()
{
    QList<W*> found;
    for ( auto* widget : QApplication::allWidgets() ) {
        if ( auto* typed = qobject_cast<W*>( widget ) ) {
            found << typed;
        }
    }
    return found;
}

/// The plugin's dialog, which its menu entry and its tab show.
serial_monitor::PortDialog* portDialog()
{
    const auto dialogs = widgetsOf<serial_monitor::PortDialog>();
    REQUIRE( dialogs.size() == 1 );
    return dialogs.first();
}

/// Nothing is left of a plugin instance: no tab, no widget, no host table.
void requireNothingLeft( const FakeHost& host )
{
    REQUIRE( host.sidebarTabs.isEmpty() );
    REQUIRE( widgetsOf<serial_monitor::SidebarWidget>().isEmpty() );
    REQUIRE( widgetsOf<serial_monitor::PortDialog>().isEmpty() );
    REQUIRE( serial_monitor::hostApiSize() == 0 );
}

/// Save @p profile in the host's serial.ini, as the dialog does.
void saveProfile( const FakeHost& host, const serial_monitor::PortProfile& profile )
{
    REQUIRE( serial_monitor::ProfileStore( host.configDir() ).upsert( profile ).isEmpty() );
}

/// The tab the plugin registered.
serial_monitor::SidebarWidget* sidebarTab( const FakeHost& host )
{
    REQUIRE( host.sidebarTabs.size() == 1 );
    return static_cast<serial_monitor::SidebarWidget*>( host.sidebarTabs.first() );
}

serial_monitor::PortProfile profileFor( const QString& name, const QString& port,
                                        const QString& logDir = {} )
{
    serial_monitor::PortProfile profile;
    profile.name = name;
    profile.serial.portName = port;
    profile.logDir = logDir;
    return profile;
}

QString stateOf( const serial_monitor::ConnectionTile* tile )
{
    REQUIRE( tile );
    return tile->findChild<QLabel*>( "state" )->property( "state" ).toString();
}

} // namespace

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
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        REQUIRE( host.menuActions.size() == 1 );

        QWidget mainWindow;
        mainWindow.show();
        mainWindow.activateWindow();
        REQUIRE(
            waitFor( [ &mainWindow ]() { return QApplication::activeWindow() == &mainWindow; } ) );

        WHEN( "the user opens the dialog from the Plugins menu" )
        {
            host.menuActions.first().trigger();
            auto* dialog = portDialog();

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
        REQUIRE( widgetsOf<serial_monitor::PortDialog>().isEmpty() );
    }
}

SCENARIO( "the Configure dialog edits the default settings", "[plugin]" )
{
    GIVEN( "an initialised plugin with timestamps on by default" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

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
                auto* dialog = portDialog();
                dialog->newConfiguration();
                auto* dialogTimestamps = dialog->findChild<QCheckBox*>( "timestamps" );
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
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

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
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
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
                REQUIRE( host.logLevels.at( host.logs.indexOf( host.notifications.first() ) )
                         == LOGSQUIRL_LOG_WARNING );
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
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
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
                const auto logged = host.logs.indexOf( host.notifications.first() );
                REQUIRE( logged >= 0 );
                REQUIRE( host.logs.count( host.notifications.first() ) == 1 );
                REQUIRE( host.logLevels.at( logged ) == LOGSQUIRL_LOG_ERROR );
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
    // The plugin removes the temporary files of every capture of this
    // process, also of an instance before a runtime disable or update: a
    // capture of its own here.  (The plugin's own captures write to save
    // paths whenever it has a configuration to start.)
    GIVEN( "an initialised plugin, and a stopped and a running, rotated temp-file capture" )
    {
        FakeHost host;
        serial_test::PseudoTerminal firstDevice;
        serial_test::PseudoTerminal secondDevice;
        const auto* api = host.api();
        auto* handle = host.handle();
        REQUIRE( logsquirl_plugin_init( api, handle ) == 0 );
        serial_monitor::Sessions captures( host );
        REQUIRE( captures.startSession( configFor( firstDevice.devicePath() ) ) );
        captures.stopSession( firstDevice.devicePath() );
        REQUIRE( captures.startSession( configFor( secondDevice.devicePath() ) ) );
        captures.rotateSession( secondDevice.devicePath() );
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
                REQUIRE( captures.startSession( configFor( firstDevice.devicePath() ) ) );
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
    GIVEN( "a stopped and a rotated tile's capture that write to save paths below the temporary "
           "root" )
    {
        FakeHost host;
        serial_test::PseudoTerminal firstDevice;
        serial_test::PseudoTerminal secondDevice;
        const auto logDir = host.tempRoot() + "/logs";
        saveProfile( host, profileFor( "stopped", firstDevice.devicePath(), logDir ) );
        saveProfile( host, profileFor( "rotated", secondDevice.devicePath(), logDir ) );
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        auto* sidebar = sidebarTab( host );
        sidebar->tile( "stopped" )->findChild<QPushButton*>( "startStop" )->click();
        REQUIRE( stateOf( sidebar->tile( "stopped" ) ) == "running" );
        sidebar->tile( "stopped" )->findChild<QPushButton*>( "startStop" )->click();
        REQUIRE( stateOf( sidebar->tile( "stopped" ) ) == "stopped" );
        sidebar->tile( "rotated" )->findChild<QPushButton*>( "startStop" )->click();
        sidebar->tile( "rotated" )->findChild<QPushButton*>( "rotate" )->click();
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
            REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
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
            REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
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
            REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
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
            REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

            THEN( "its sidebar widget is registered as a tab" )
            {
                REQUIRE( host.sidebarTabs.size() == 1 );
                REQUIRE( widgetsOf<serial_monitor::SidebarWidget>()
                         == QList<serial_monitor::SidebarWidget*>{ sidebarTab( host ) } );
            }

            AND_WHEN( "it is shut down" )
            {
                bool widgetLivedOnUnregister = false;
                host.onUnregisterSidebarTab = [ &host, &widgetLivedOnUnregister ]( void* widget ) {
                    widgetLivedOnUnregister
                        = widgetsOf<serial_monitor::SidebarWidget>()
                              == QList<serial_monitor::SidebarWidget*>{ sidebarTab( host ) }
                          && widget == host.sidebarTabs.first();
                };
                logsquirl_plugin_shutdown();

                THEN( "the tab is unregistered while its widget lives, and nothing is left" )
                {
                    REQUIRE( widgetLivedOnUnregister );
                    requireNothingLeft( host );
                }
            }

            logsquirl_plugin_shutdown();
        }
    }

    GIVEN( "a plugin that was never initialised" )
    {
        FakeHost host;

        THEN( "shutting it down is harmless" )
        {
            REQUIRE_NOTHROW( logsquirl_plugin_shutdown() );
            requireNothingLeft( host );
            REQUIRE( host.logs.isEmpty() );
        }
    }
}

SCENARIO( "saved configurations survive a restart of LogSquirl", "[plugin]" )
{
    GIVEN( "a configuration saved in the dialog of an initialised plugin" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        auto* dialog = portDialog();
        dialog->findChild<QComboBox*>( "profile" )->setEditText( "board" );
        dialog->findChild<QComboBox*>( "port" )->setEditText( "/dev/ttyUSB0" );
        dialog->findChild<QPushButton*>( "saveProfile" )->click();
        REQUIRE( sidebarTab( host )->tile( "board" ) );

        WHEN( "the plugin is shut down and started again, as when LogSquirl restarts" )
        {
            logsquirl_plugin_shutdown();
            REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

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
        saveProfile( host, profileFor( "board", "/dev/ttyUSB0" ) );
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        auto* dialog = portDialog();

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
        saveProfile( host, profileFor( "board", device.devicePath(), host.tempRoot() + "/logs" ) );
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        sidebarTab( host )->tile( "board" )->findChild<QPushButton*>( "startStop" )->click();
        REQUIRE( stateOf( sidebarTab( host )->tile( "board" ) ) == "running" );
        REQUIRE( host.followedFiles.size() == 1 );

        WHEN( "its configuration is renamed in the dialog while it captures" )
        {
            sidebarTab( host )->tile( "board" )->findChild<QPushButton*>( "edit" )->click();
            auto* dialog = portDialog();
            dialog->findChild<QComboBox*>( "profile" )->setEditText( "console" );
            dialog->findChild<QPushButton*>( "saveProfile" )->click();

            THEN( "the plugin's store refuses, the capture asked whether it runs" )
            {
                REQUIRE( dialog->findChild<QLabel*>( "status" )->text().contains( "board" ) );
                const serial_monitor::ProfileStore saved( host.configDir() );
                REQUIRE( saved.find( "board" ) );
                REQUIRE_FALSE( saved.find( "console" ) );
                REQUIRE( stateOf( sidebarTab( host )->tile( "board" ) ) == "running" );
            }
        }

        WHEN( "the plugin is shut down" )
        {
            // What the parts show as the host lets go of the tab, the first
            // of them to go.
            QString tileState;
            int dialogs = 0;
            host.onUnregisterSidebarTab = [ &host, &tileState, &dialogs ]( void* ) {
                tileState = stateOf( sidebarTab( host )->tile( "board" ) );
                dialogs = widgetsOf<serial_monitor::PortDialog>().size();
            };
            logsquirl_plugin_shutdown();

            THEN( "the capture stopped before the parts that show it went" )
            {
                REQUIRE( tileState == "stopped" );
                REQUIRE( dialogs == 1 );
                requireNothingLeft( host );
            }

            THEN( "the capture's file stays for its tab" )
            {
                REQUIRE( QFileInfo::exists( host.followedFiles.first() ) );
            }

            THEN( "the port is free for another capture" )
            {
                REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
                sidebarTab( host )
                    ->tile( "board" )
                    ->findChild<QPushButton*>( "startStop" )
                    ->click();
                REQUIRE( stateOf( sidebarTab( host )->tile( "board" ) ) == "running" );
                logsquirl_plugin_shutdown();
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
            REQUIRE_NOTHROW( rc = logsquirl_plugin_init( host.api(), host.handle() ) );

            THEN( "initialisation fails, is logged, and leaves nothing behind" )
            {
                REQUIRE( rc != 0 );
                requireNothingLeft( host );
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
            bool widgetLivedOnUnregister = false;
            host.onUnregisterSidebarTab = [ &widgetLivedOnUnregister ]( void* ) {
                widgetLivedOnUnregister = widgetsOf<serial_monitor::SidebarWidget>().size() == 1;
            };
            int rc = 0;
            REQUIRE_NOTHROW( rc = logsquirl_plugin_init( host.api(), host.handle() ) );

            THEN( "initialisation fails, is logged, and the tab is unregistered before its "
                  "widget goes" )
            {
                REQUIRE( rc != 0 );
                REQUIRE( widgetLivedOnUnregister );
                requireNothingLeft( host );
                REQUIRE_FALSE(
                    host.logs.filter( "initialisation failed: the log is full" ).isEmpty() );
            }
        }

        REQUIRE_NOTHROW( logsquirl_plugin_shutdown() );
    }

    GIVEN( "an initialised plugin in a host whose log throws on shutdown" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.failLogContaining = "shutting down";

        WHEN( "it is shut down" )
        {
            REQUIRE_NOTHROW( logsquirl_plugin_shutdown() );

            THEN( "it is logged, and every other step still ran" )
            {
                REQUIRE_FALSE( host.logs.filter( "failed: the log is full" ).isEmpty() );
                requireNothingLeft( host );
            }
        }
    }

    GIVEN( "an initialised plugin in a host whose unregister_sidebar_tab() throws" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.onUnregisterSidebarTab
            = []( void* ) { throw std::runtime_error( "the tab is stuck" ); };

        WHEN( "it is shut down" )
        {
            REQUIRE_NOTHROW( logsquirl_plugin_shutdown() );

            THEN( "it is logged, and every part still goes" )
            {
                REQUIRE_FALSE( host.logs.filter( "failed: the tab is stuck" ).isEmpty() );
                REQUIRE( widgetsOf<serial_monitor::SidebarWidget>().isEmpty() );
                REQUIRE( widgetsOf<serial_monitor::PortDialog>().isEmpty() );
                REQUIRE( serial_monitor::hostApiSize() == 0 );
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
            REQUIRE(
                logsquirl_plugin_init_ex( host.api(), host.handle(), sizeof( LogSquirlHostApi ) )
                == 0 );

            THEN( "it keeps the size, and every later function counts as there" )
            {
                REQUIRE( serial_monitor::hostApiSize() == sizeof( LogSquirlHostApi ) );
                REQUIRE( LOGSQUIRL_HOST_API_HAS( serial_monitor::hostApiSize(), open_regex_lab ) );
            }

            AND_WHEN( "it is shut down" )
            {
                logsquirl_plugin_shutdown();

                THEN( "the size is forgotten" )
                {
                    REQUIRE( serial_monitor::hostApiSize() == 0 );
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
            REQUIRE( logsquirl_plugin_init( baseTable, host.handle() ) == 0 );

            THEN( "the plugin starts with the base table's size and no later function" )
            {
                REQUIRE( serial_monitor::hostApiSize() == LOGSQUIRL_HOST_API_BASE_SIZE );
                REQUIRE_FALSE(
                    LOGSQUIRL_HOST_API_HAS( serial_monitor::hostApiSize(), open_regex_lab ) );
                REQUIRE( host.sidebarTabs.size() == 1 );
            }

            logsquirl_plugin_shutdown();
        }
    }
}
