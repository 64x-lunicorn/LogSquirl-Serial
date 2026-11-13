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

#include "host.h"
#include "logsquirl_plugin_api.h"

#include <QString>

#include <cstddef>
#include <utility>

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
    const LogSquirlHostApi* api = nullptr; ///< Host API function table.
    void* handle = nullptr;                ///< Opaque plugin instance handle.
    /// The Host the capture modules are handed: api and handle between
    /// init() and shutdown(), detached (doing nothing) otherwise.
    CApiHost host;
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
    /// Where init() puts the temporary directories (host.tempRoot()); empty:
    /// the system's.  Set by the tests, before init().
    QString tempRoot;
};

/// The single global instance — defined in plugin.cpp.
extern PluginState g_state;

/// guarded() through the plugin's host (g_state.host): entry points and the
/// slots of the parts not handed a Host (the dialog) go through it.
template <typename Work>
bool guarded( const char* what, Work&& work ) noexcept
{
    return guarded( g_state.host, what, std::forward<Work>( work ) );
}

} // namespace serial_monitor
