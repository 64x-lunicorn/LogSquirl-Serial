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
#include "sidebarwidget.h"
#include "serialprocess.h"

#include <QInputDialog>
#include <QSettings>

// ── Global state ─────────────────────────────────────────────────────────

namespace serial_monitor {
PluginState g_state;

void hostLog( int level, const char* message )
{
    if ( g_state.api && g_state.handle ) {
        g_state.api->log_message( g_state.handle, level, message );
    }
}
} // namespace serial_monitor

// ── Plugin metadata ──────────────────────────────────────────────────────

/// Static plugin info.  All strings have static storage duration.
static const LogSquirlPluginInfo kPluginInfo = {
    /* id          */ "io.github.logsquirl.serial",
    /* name        */ "Serial Monitor",
    /* version     */ "0.4.0",
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
    serial_monitor::g_state.dialog->show();
    serial_monitor::g_state.dialog->raise();
    serial_monitor::g_state.dialog->activateWindow();
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

    api->log_message( handle, LOGSQUIRL_LOG_INFO, "Serial Monitor plugin initialising\u2026" );

    // Add "Serial Monitor…" to the Plugins menu.  When clicked it opens
    // a non-modal dialog for port selection and session management.
    api->register_menu_action( handle, "Plugins", "Serial Monitor\u2026",
                               &showSerialDialog, nullptr );

    // Create the PortWidget early so the sidebar panel can reference it.
    serial_monitor::g_state.dialog = new serial_monitor::PortWidget();

    // Register a sidebar tab for serial session management
    serial_monitor::g_state.sidebarWidget
        = new serial_monitor::SidebarWidget( serial_monitor::g_state.dialog );
    api->register_sidebar_tab(
        handle, "Serial",
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
        serial_monitor::g_state.dialog->stopAll( true );
        delete serial_monitor::g_state.dialog;
        serial_monitor::g_state.dialog = nullptr;
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
 * Lets the user change the default baud rate and timestamp preference.
 */
LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_configure( void* parent_widget )
{
    auto* parent = static_cast<QWidget*>( parent_widget );

    const auto configDir = serial_monitor::SerialProcess::configDir();
    QSettings settings( configDir + "/serial.ini", QSettings::IniFormat );
    const auto currentBaud = settings.value( "serial/defaultBaud", 115200 ).toInt();
    const auto currentTimestamps = settings.value( "serial/timestamps", true ).toBool();

    const auto prompt
        = QString( "Default baud rate: %1\nTimestamps: %2\n\n"
                   "Enter new default baud rate (leave empty to keep %1):" )
              .arg( currentBaud )
              .arg( currentTimestamps ? "enabled" : "disabled" );

    bool ok = false;
    const auto newBaud
        = QInputDialog::getInt( parent, "Configure Serial Monitor",
                                prompt, currentBaud, 300, 4000000, 1, &ok );

    if ( ok ) {
        settings.setValue( "serial/defaultBaud", newBaud );
        serial_monitor::hostLog(
            LOGSQUIRL_LOG_INFO,
            qPrintable( QString( "Default baud rate set to %1" ).arg( newBaud ) ) );
    }
}

} // extern "C"
