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
 * @brief What the rest of the plugin may ask of the running plugin instance.
 *
 * The LogSquirl plugin ABI is pure C: the host calls free functions
 * (logsquirl_plugin_init_ex(), logsquirl_plugin_shutdown(), …) with no
 * object context.  init() creates one plugin instance, which owns every
 * part (plugin.cpp), and the entry points reach it through the one global
 * pointer to it; shutdown() destroys it.  All of it happens on the main
 * (GUI) thread, so no locking is necessary.
 */

#pragma once

#include "host.h"

#include <QString>

#include <cstddef>
#include <utility>

namespace serial_monitor {

/// The Host of the running plugin instance; between instances, a detached
/// one, which logs, notifies and opens nothing.
Host& pluginHost() noexcept;

/// The size of the host's table (init_ex()) while the plugin runs, 0
/// otherwise: a function added after LogSquirl 26.10 may be used only if
/// LOGSQUIRL_HOST_API_HAS( hostApiSize(), member ) says the running host
/// has it.  Never copy the table.
std::size_t hostApiSize() noexcept;

/// Where the next init() puts the temporary directories (Host::tempRoot());
/// empty: the system's.  For the tests.
void setTempRootForTests( const QString& tempRoot );

/// guarded() through the plugin's host (pluginHost()): entry points and the
/// slots of the parts not handed a Host (the dialog) go through it.
template <typename Work>
bool guarded( const char* what, Work&& work ) noexcept
{
    return guarded( pluginHost(), what, std::forward<Work>( work ) );
}

} // namespace serial_monitor
