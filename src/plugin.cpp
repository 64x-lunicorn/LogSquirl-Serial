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
 * @file plugin.cpp
 * @brief C ABI entry points for the LogSquirl serial monitor plugin.
 *
 * This file implements the four exported symbols that every LogSquirl
 * plugin must provide:
 *
 *   - logsquirl_plugin_get_info()   → static metadata
 *   - logsquirl_plugin_init_ex()    → init() for LogSquirl 26.11 and later,
 *                                     with the size of the host's table
 *   - logsquirl_plugin_init()       → store host API, register menu action
 *   - logsquirl_plugin_shutdown()   → tear down sessions, destroy dialog
 *   - logsquirl_plugin_configure()  → open default settings dialog
 *
 * PLUGIN LIFECYCLE
 * ────────────────
 *   1. Host calls get_info() to read metadata.
 *   2. Host calls init_ex(api, handle, api_size) — or init(api, handle),
 *      which is init_ex() with the base table's size, if it is older than
 *      LogSquirl 26.11 — we store the pointers and the size, register
 *      a menu action that opens the configuration dialog, create the
 *      Sessions, the ProfileStore of the saved port configurations, the
 *      PortDialog that edits them and the SidebarWidget with a tile for each,
 *      and register the latter as a sidebar tab.
 *   3. User saves configurations in the dialog, and starts and stops their
 *      captures from their tiles.
 *   4. Host calls shutdown() — we unregister + delete the sidebar, stop all
 *      sessions, destroy the dialog, the store and the sessions, and clear
 *      state.
 *
 * No exception may leave an entry point or a host callback: the host is C,
 * and an exception escaping into it terminates LogSquirl.  Their work runs
 * through guarded(), which logs the failure instead.
 *
 * @see logsquirl_plugin_api.h for the full host API reference.
 */

#include "plugin.h"

#include "portdialog.h"
#include "profiles.h"
#include "serialprocess.h"
#include "sessions.h"
#include "sidebarwidget.h"
#include "tempdirs.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QWindow>

#include <exception>

// ── Global state ─────────────────────────────────────────────────────────

namespace serial_monitor {
PluginState g_state;

void hostLog( int level, const QString& message )
{
    if ( g_state.api && g_state.handle ) {
        g_state.api->log_message( g_state.handle, level, message.toUtf8().constData() );
    }
}

void hostNotify( const QString& message )
{
    if ( g_state.api && g_state.handle ) {
        g_state.api->show_notification( g_state.handle, message.toUtf8().constData() );
    }
}

void logFailure( const char* what, const char* reason ) noexcept
{
    try {
        hostLog( LOGSQUIRL_LOG_ERROR, QStringLiteral( "Serial Monitor plugin: %1 failed: %2" )
                                          .arg( what, QString::fromUtf8( reason ) ) );
    } catch ( ... ) {
        // Nothing left to report it with.
    }
}
} // namespace serial_monitor

using serial_monitor::guarded;

// ── Plugin metadata ──────────────────────────────────────────────────────

/// Static plugin info.  All strings have static storage duration.
static const LogSquirlPluginInfo kPluginInfo = {
    /* id          */ "io.github.logsquirl.serial",
    /* name        */ "Serial Monitor",
    /* version     */ LOGSQUIRL_PLUGIN_VERSION,
    /* description */ "Stream serial port data into LogSquirl tabs",
    /* author      */ "LogSquirl Contributors",
    /* license     */ "GPL-3.0-or-later",
    /* type        */ LOGSQUIRL_PLUGIN_UI,
    /* api_version */ LOGSQUIRL_PLUGIN_API_VERSION,
};

// ── Menu action callback ─────────────────────────────────────────────────

/// Show the configuration dialog, on top of the window it was opened from.
static void showDialog()
{
    auto* dialog = serial_monitor::g_state.dialog;
    if ( !dialog ) {
        return; // not initialised: nothing to show
    }

    // The dialog is created parentless in init() and deleted in shutdown(),
    // so it must not become a child of a main window that may be destroyed
    // first.  A transient parent keeps it on top of the window whose menu
    // opened it, without handing over ownership.
    auto* window = QApplication::activeWindow();
    if ( window && window != dialog ) {
        dialog->winId(); // creates the native window, and so windowHandle()
        dialog->windowHandle()->setTransientParent( window->windowHandle() );
    }

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

/// Called when the user clicks "Serial Monitor…" in the Plugins menu: the
/// dialog as it was left.
static void showSerialDialog( void* /* userData */ )
{
    guarded( "showing the Serial Monitor dialog", [] { showDialog(); } );
}

/// Create the sidebar tab and connect it to the dialog, which opens for
/// what is asked in the tab: a new configuration, or a tile's.  Both follow
/// the profile store.
static void createSidebar()
{
    auto& st = serial_monitor::g_state;
    auto* sidebar = new serial_monitor::SidebarWidget( st.profiles, st.sessions );
    st.sidebarWidget = sidebar;
    auto* dialog = st.dialog;
    QObject::connect( sidebar, &serial_monitor::SidebarWidget::newConfigurationRequested, dialog,
                      [ dialog ] {
                          guarded( "opening a new serial configuration", [ dialog ] {
                              dialog->newConfiguration();
                              showDialog();
                          } );
                      } );
    QObject::connect( sidebar, &serial_monitor::SidebarWidget::editRequested, dialog,
                      [ dialog ]( const QString& name ) {
                          guarded( "editing a serial configuration", [ dialog, &name ] {
                              dialog->editConfiguration( name );
                              showDialog();
                          } );
                      } );
}

/// Let the user change the defaults (baud rate and timestamps of a new
/// configuration, the size at which a log file is rotated) and save them.
static void configureDefaults( QWidget* parent )
{
    auto* store = serial_monitor::g_state.profiles;
    if ( !store ) {
        return; // not initialised: nothing to configure
    }
    // Without a configuration directory nothing can be saved.
    if ( const auto problem = store->defaultsUnsavable(); !problem.isEmpty() ) {
        serial_monitor::hostLog( LOGSQUIRL_LOG_WARNING, problem );
        serial_monitor::hostNotify( problem );
        return;
    }

    const auto defaults = store->defaults();

    QDialog dialog( parent );
    dialog.setWindowTitle( "Configure Serial Monitor" );
    auto* layout = new QFormLayout( &dialog );

    auto* baudSpin = new QSpinBox( &dialog );
    baudSpin->setRange( 300, 4000000 );
    baudSpin->setValue( defaults.baudRate );
    layout->addRow( "Default baud rate:", baudSpin );

    auto* timestampCheckBox = new QCheckBox( "Prepend timestamp to each line", &dialog );
    timestampCheckBox->setChecked( defaults.timestamps );
    layout->addRow( timestampCheckBox );

    auto* maxFileSpin = new QSpinBox( &dialog );
    maxFileSpin->setObjectName( "maxFileMiB" );
    maxFileSpin->setRange( 0, 1024 * 1024 ); // up to 1 TiB
    maxFileSpin->setSuffix( " MiB" );
    maxFileSpin->setSpecialValueText( "Never" );
    maxFileSpin->setValue( defaults.maxFileMiB );
    maxFileSpin->setToolTip( "When a log file reaches this size, the capture goes on in a new "
                             "file, opened in a new tab" );
    layout->addRow( "Start a new log file at:", maxFileSpin );

    auto* buttons
        = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
    QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
    QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
    layout->addRow( buttons );

    if ( dialog.exec() != QDialog::Accepted ) {
        return;
    }

    serial_monitor::Defaults chosen;
    chosen.baudRate = baudSpin->value();
    chosen.timestamps = timestampCheckBox->isChecked();
    chosen.maxFileMiB = maxFileSpin->value();
    if ( const auto problem = store->saveDefaults( chosen ); !problem.isEmpty() ) {
        serial_monitor::hostLog( LOGSQUIRL_LOG_ERROR, problem );
        serial_monitor::hostNotify( problem );
        return;
    }
    serial_monitor::hostLog( LOGSQUIRL_LOG_INFO,
                             QString( "Default baud rate set to %1, timestamps %2" )
                                 .arg( baudSpin->value() )
                                 .arg( timestampCheckBox->isChecked() ? "on" : "off" ) );
    // A new configuration proposes them from now on (ProfileStore::newProfile());
    // the saved ones keep their own settings.
}

// ── Exported C entry points ──────────────────────────────────────────────

extern "C" {

LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_shutdown( void );

/**
 * Return static plugin metadata.
 *
 * The host calls this before init() to read the plugin's identity, type,
 * and API version.  The returned pointer is valid for the lifetime of the
 * shared library.
 */
LOGSQUIRL_PLUGIN_EXPORT const LogSquirlPluginInfo* logsquirl_plugin_get_info( void )
{
    return &kPluginInfo;
}

/**
 * Initialise the plugin.
 *
 * @param api     Host API function table — valid until shutdown().
 * @param handle  Opaque handle — pass back to every host API call.
 * @param api_size  The size of the table @p api points to, in the host's
 *                  build: it tells which later functions the host has.
 * @return 0 on success, non-zero on failure.
 *
 * Stores the host API and handle, registers a menu action that opens the
 * serial monitor dialog, and a sidebar tab.  A failure is logged and undone
 * (shutdown()), and init returns 1.
 */
LOGSQUIRL_PLUGIN_EXPORT int logsquirl_plugin_init_ex( const LogSquirlHostApi* api, void* handle,
                                                      size_t api_size )
{
    if ( !api || !handle ) {
        return 1;
    }

    serial_monitor::g_state.hostApiSize = api_size;

    serial_monitor::g_state.api = api;
    serial_monitor::g_state.handle = handle;
    serial_monitor::g_state.initialised = true;
    serial_monitor::g_state.quitting = false;

    const bool ok = guarded( "initialisation", [ api, handle ] {
        api->log_message( handle, LOGSQUIRL_LOG_INFO, "Serial Monitor plugin initialising\u2026" );

        // Files of LogSquirl processes that ended without removing them,
        // e.g. after a crash: no tab can show them any more.
        serial_monitor::removeStaleTempDirs( serial_monitor::tempRoot() );

        // Add "Serial Monitor…" to the Plugins menu.  When clicked it opens
        // the non-modal dialog that edits the saved port configurations.
        api->register_menu_action( handle, "Plugins", "Serial Monitor\u2026", &showSerialDialog,
                                   nullptr );

        // The captures, the saved configurations, and the dialog that edits
        // them (shown from the menu and the sidebar).  Creating the sessions
        // scans the ports, once, for every view.
        auto* sessions = new serial_monitor::Sessions();
        serial_monitor::g_state.sessions = sessions;
        serial_monitor::g_state.profiles = new serial_monitor::ProfileStore(
            QString::fromUtf8( api->get_config_dir( handle ) ),
            [ sessions ]( const QString& name ) { return !sessions->portOf( name ).isEmpty(); } );
        serial_monitor::g_state.dialog
            = new serial_monitor::PortDialog( serial_monitor::g_state.profiles, sessions );

        // The host shuts the plugin down both when LogSquirl quits (after
        // aboutToQuit) and when the plugin is disabled or updated at runtime,
        // with the tabs left open; only in the first case may the temporary
        // files go.
        if ( auto* app = QCoreApplication::instance() ) {
            QObject::connect( app, &QCoreApplication::aboutToQuit, sessions,
                              []() { serial_monitor::g_state.quitting = true; } );
        }

        // Register the sidebar tab: one tile per saved configuration
        createSidebar();
        api->register_sidebar_tab( handle, "Serial",
                                   static_cast<void*>( serial_monitor::g_state.sidebarWidget ) );
        serial_monitor::g_state.sidebarTabRegistered = true;

        api->log_message( handle, LOGSQUIRL_LOG_INFO, "Serial Monitor plugin ready." );
    } );

    if ( !ok ) {
        // Undo what was set up, in reverse: shutdown() unregisters the tab
        // only if the host took it, then deletes the widgets.
        logsquirl_plugin_shutdown();
        return 1;
    }
    return 0;
}

/**
 * Initialise the plugin for a host older than init_ex(): its table ends
 * where the functions LogSquirl 26.11 added begin.  The host refuses a
 * plugin without this entry point.
 */
LOGSQUIRL_PLUGIN_EXPORT int logsquirl_plugin_init( const LogSquirlHostApi* api, void* handle )
{
    return logsquirl_plugin_init_ex( api, handle, LOGSQUIRL_HOST_API_BASE_SIZE );
}

/**
 * Shut down the plugin — stop all sessions and release resources.
 *
 * The host calls this before unloading the shared library.  After this
 * function returns, no host API calls may be made.
 */
LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_shutdown( void )
{
    auto& st = serial_monitor::g_state;
    // Each step on its own, so that one failing does not skip the others.
    guarded( "shutdown", [] {
        serial_monitor::hostLog( LOGSQUIRL_LOG_INFO, "Serial Monitor plugin shutting down\u2026" );
    } );

    // The host must let go of the tab before its widget is deleted.
    if ( st.sidebarTabRegistered ) {
        st.sidebarTabRegistered = false;
        guarded( "unregistering the sidebar tab", [] {
            auto& st = serial_monitor::g_state;
            if ( st.api && st.handle ) {
                st.api->unregister_sidebar_tab( st.handle, static_cast<void*>( st.sidebarWidget ) );
            }
        } );
    }
    if ( st.sidebarWidget ) {
        guarded( "shutdown", [] { delete serial_monitor::g_state.sidebarWidget; } );
        st.sidebarWidget = nullptr;
    }

    // The sessions stop while the dialog still uses them, and go after it.
    if ( st.sessions ) {
        guarded( "stopping the serial sessions",
                 [] { serial_monitor::g_state.sessions->stopAll(); } );
    }
    if ( st.dialog ) {
        guarded( "shutdown", [] { delete serial_monitor::g_state.dialog; } );
        st.dialog = nullptr;
    }
    // After the tab and the dialog, which use it; before the sessions it asks.
    if ( st.profiles ) {
        guarded( "shutdown", [] { delete serial_monitor::g_state.profiles; } );
        st.profiles = nullptr;
    }
    if ( st.sessions ) {
        guarded( "shutdown", [] { delete serial_monitor::g_state.sessions; } );
        st.sessions = nullptr;
    }

    // The tabs close with LogSquirl: remove the files of every instance of
    // the plugin in this process, also those of instances before a runtime
    // disable or update, which only the directory names remember.  Save
    // paths and the log directory are never touched.
    if ( st.quitting ) {
        guarded( "removing temporary files",
                 [] { serial_monitor::removeOwnTempDirs( serial_monitor::tempRoot() ); } );
    }

    serial_monitor::g_state.api = nullptr;
    serial_monitor::g_state.handle = nullptr;
    serial_monitor::g_state.hostApiSize = 0;
    serial_monitor::g_state.initialised = false;
}

/**
 * Open a configuration dialog for default serial settings.
 *
 * @param parent_widget  Cast of a QWidget* the plugin can use as dialog parent.
 *
 * Lets the user change the default baud rate and whether lines are
 * timestamped, which a new configuration proposes, and the size at which a
 * log file is rotated, for every capture.
 */
LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_configure( void* parent_widget )
{
    guarded( "configuring",
             [ parent_widget ] { configureDefaults( static_cast<QWidget*>( parent_widget ) ); } );
}

} // extern "C"
