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
 *   - logsquirl_plugin_init()       → store host API, register menu action
 *   - logsquirl_plugin_shutdown()   → tear down sessions, destroy dialog
 *   - logsquirl_plugin_configure()  → open default settings dialog
 *
 * PLUGIN LIFECYCLE
 * ────────────────
 *   1. Host calls get_info() to read metadata.
 *   2. Host calls init(api, handle) — we store the pointers and register
 *      a menu action that opens the serial monitor dialog.
 *   3. User interacts with the dialog (select port, configure, start, stop).
 *   4. Host calls shutdown() — we stop all sessions, destroy the dialog,
 *      and clear state.
 *
 * @see logsquirl_plugin_api.h for the full host API reference.
 */

#include "plugin.h"

#include "portwidget.h"
#include "serialprocess.h"
#include "sidebarwidget.h"
#include "tempdirs.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSettings>
#include <QSpinBox>
#include <QWindow>

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
} // namespace serial_monitor

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

/**
 * Called when the user clicks "Serial Monitor…" in the Plugins menu.
 * Creates (if needed) and shows the serial monitor dialog.
 */
static void showSerialDialog( void* /* userData */ )
{
    if ( !serial_monitor::g_state.dialog ) {
        serial_monitor::g_state.dialog = new serial_monitor::PortWidget();
    }
    auto* dialog = serial_monitor::g_state.dialog;

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

// ── Exported C entry points ──────────────────────────────────────────────

extern "C" {

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
 * @return 0 on success, non-zero on failure.
 *
 * Stores the host API and handle, then registers a menu action that
 * opens the serial monitor dialog.
 */
LOGSQUIRL_PLUGIN_EXPORT int logsquirl_plugin_init( const LogSquirlHostApi* api, void* handle )
{
    if ( !api || !handle ) {
        return 1;
    }

    serial_monitor::g_state.api = api;
    serial_monitor::g_state.handle = handle;
    serial_monitor::g_state.initialised = true;
    serial_monitor::g_state.quitting = false;

    api->log_message( handle, LOGSQUIRL_LOG_INFO, "Serial Monitor plugin initialising\u2026" );

    // Files of LogSquirl processes that ended without removing them,
    // e.g. after a crash: no tab can show them any more.
    serial_monitor::removeStaleTempDirs( serial_monitor::tempRoot() );

    // Add "Serial Monitor…" to the Plugins menu.  When clicked it opens
    // a non-modal dialog for port selection and session management.
    api->register_menu_action( handle, "Plugins", "Serial Monitor\u2026", &showSerialDialog,
                               nullptr );

    // Create the PortWidget early so the sidebar panel can reference it.
    serial_monitor::g_state.dialog = new serial_monitor::PortWidget();

    // The host shuts the plugin down both when LogSquirl quits (after
    // aboutToQuit) and when the plugin is disabled or updated at runtime,
    // with the tabs left open; only in the first case may the temporary
    // files go.
    if ( auto* app = QCoreApplication::instance() ) {
        QObject::connect( app, &QCoreApplication::aboutToQuit, serial_monitor::g_state.dialog,
                          []() { serial_monitor::g_state.quitting = true; } );
    }

    // Register a sidebar tab for serial session management
    serial_monitor::g_state.sidebarWidget
        = new serial_monitor::SidebarWidget( serial_monitor::g_state.dialog );
    api->register_sidebar_tab( handle, "Serial",
                               static_cast<void*>( serial_monitor::g_state.sidebarWidget ) );

    api->log_message( handle, LOGSQUIRL_LOG_INFO, "Serial Monitor plugin ready." );
    return 0;
}

/**
 * Shut down the plugin — stop all sessions and release resources.
 *
 * The host calls this before unloading the shared library.  After this
 * function returns, no host API calls may be made.
 */
LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_shutdown( void )
{
    serial_monitor::hostLog( LOGSQUIRL_LOG_INFO, "Serial Monitor plugin shutting down\u2026" );

    if ( serial_monitor::g_state.sidebarWidget ) {
        serial_monitor::g_state.api->unregister_sidebar_tab(
            serial_monitor::g_state.handle,
            static_cast<void*>( serial_monitor::g_state.sidebarWidget ) );
        delete serial_monitor::g_state.sidebarWidget;
        serial_monitor::g_state.sidebarWidget = nullptr;
    }

    if ( serial_monitor::g_state.dialog ) {
        serial_monitor::g_state.dialog->stopAll();
        delete serial_monitor::g_state.dialog;
        serial_monitor::g_state.dialog = nullptr;
    }

    // The tabs close with LogSquirl: remove the files of every instance of
    // the plugin in this process, also those of instances before a runtime
    // disable or update, which only the directory names remember.  Save
    // paths and the log directory are never touched.
    if ( serial_monitor::g_state.quitting ) {
        serial_monitor::removeOwnTempDirs( serial_monitor::tempRoot() );
    }

    serial_monitor::g_state.api = nullptr;
    serial_monitor::g_state.handle = nullptr;
    serial_monitor::g_state.initialised = false;
}

/**
 * Open a configuration dialog for default serial settings.
 *
 * @param parent_widget  Cast of a QWidget* the plugin can use as dialog parent.
 *
 * Lets the user change the default baud rate and whether lines are
 * timestamped, and applies them to the open Serial Monitor panels.
 */
LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_configure( void* parent_widget )
{
    auto* parent = static_cast<QWidget*>( parent_widget );
    const auto defaults = serial_monitor::SerialProcess::defaultConfig();

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

    auto* buttons
        = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
    QObject::connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
    QObject::connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
    layout->addRow( buttons );

    if ( dialog.exec() != QDialog::Accepted ) {
        return;
    }

    const auto configDir = serial_monitor::SerialProcess::configDir();
    QSettings settings( configDir + "/serial.ini", QSettings::IniFormat );
    settings.setValue( "serial/defaultBaud", baudSpin->value() );
    settings.setValue( "serial/timestamps", timestampCheckBox->isChecked() );
    settings.sync();
    serial_monitor::hostLog( LOGSQUIRL_LOG_INFO,
                             QString( "Default baud rate set to %1, timestamps %2" )
                                 .arg( baudSpin->value() )
                                 .arg( timestampCheckBox->isChecked() ? "on" : "off" ) );

    // Show the new defaults in the open panels
    if ( serial_monitor::g_state.dialog ) {
        serial_monitor::g_state.dialog->loadDefaults();
    }
    if ( serial_monitor::g_state.sidebarWidget ) {
        serial_monitor::g_state.sidebarWidget->loadDefaults();
    }
}

} // extern "C"
