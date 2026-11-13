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
 * @file plugin.h
 * @brief Global plugin state shared across all translation units.
 *
 * This header centralises the host API pointer, the opaque handle, and
 * the sessions and the views of them.  It is the single source of truth for "is the
 * plugin initialised?" and "what is running?".
 *
 * DESIGN NOTE — why globals and not a class?
 * ------------------------------------------
 * The LogSquirl plugin ABI is pure C.  The host calls free functions
 * (logsquirl_plugin_init, logsquirl_plugin_shutdown, …) with no object
 * context.  A small, clearly scoped set of file-level globals is the
 * idiomatic way to bridge between the C entry points and the C++/Qt
 * implementation behind them.
 */

#pragma once

#include "logsquirl_plugin_api.h"

#include <QString>

#include <cstddef>
#include <exception>

namespace serial_monitor {
class PortDialog;
class ProfileStore;
class Sessions;
class SidebarWidget;
} // namespace serial_monitor

namespace serial_monitor {

/**
 * Global plugin state.
 *
 * Populated by logsquirl_plugin_init(), cleared by logsquirl_plugin_shutdown().
 * All fields are only accessed from the main (GUI) thread, so no locking
 * is necessary.
 */
struct PluginState {
    const LogSquirlHostApi* api = nullptr;  ///< Host API function table.
    void* handle = nullptr;                 ///< Opaque plugin instance handle.
    Sessions* sessions = nullptr;           ///< The running captures, which the views show.
    ProfileStore* profiles = nullptr;       ///< serial.ini: defaults, configurations.
    PortDialog* dialog = nullptr;           ///< The dialog that edits a saved configuration.
    SidebarWidget* sidebarWidget = nullptr; ///< The sidebar tab: one tile per configuration.
    bool sidebarTabRegistered = false;      ///< The host holds sidebarWidget as a tab.
    /// The size of the host's table (init_ex()): a function added after
    /// LogSquirl 26.10 may be used only if LOGSQUIRL_HOST_API_HAS( hostApiSize,
    /// member ) says the running host has it.  Never copy *api.
    std::size_t hostApiSize = 0;
    bool initialised = false; ///< True between init() and shutdown().
    bool quitting = false; ///< LogSquirl is quitting (aboutToQuit), not just unloading the plugin.
    QString tempRoot;      ///< Where temporary directories go; empty: the system's (for tests).
};

/// The single global instance — defined in plugin.cpp.
extern PluginState g_state;

/// Log a message through the host API (no-op if not initialised).
/// The host decodes the message as UTF-8.
void hostLog( int level, const QString& message );

/// Show a host notification (no-op if not initialised).
/// The host decodes the message as UTF-8.
void hostNotify( const QString& message );

/// Log that work for the host (@p what) failed, for @p reason.
void logFailure( const char* what, const char* reason ) noexcept;

/// Run work for the host, logging instead of throwing: the host is C, and
/// an exception escaping into it would terminate LogSquirl.  Entry points
/// and every slot the host's event loop runs (a click in the sidebar or the
/// dialog) go through it.  Returns false if it threw.
template <typename Work>
bool guarded( const char* what, Work&& work ) noexcept
{
    try {
        work();
        return true;
    } catch ( const std::exception& e ) {
        logFailure( what, e.what() );
    } catch ( ... ) {
        logFailure( what, "unknown exception" );
    }
    return false;
}

} // namespace serial_monitor
