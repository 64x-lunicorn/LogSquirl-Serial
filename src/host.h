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
 * @file host.h
 * @brief What the capture modules need of LogSquirl: the Host they are handed.
 *
 * The session manager (Sessions), the serial capture (SerialProcess) and the
 * sidebar take a Host, and reach LogSquirl through it only: a log, a
 * notification, a followed tab, the configuration directory and the
 * temporary root.  Two implementations:
 *
 *   - CApiHost, the adapter over the host's C table (logsquirl_plugin_api.h),
 *     which the plugin's entry points set up (plugin.h);
 *   - the tests' FakeHost (tests/fakehost.h), which records every call.
 *
 * guarded( host, … ) runs work for the host's event loop, logging an
 * exception through @p host instead of letting it escape into C.
 */

#pragma once

#include "logsquirl_plugin_api.h"

#include <QString>

#include <exception>

namespace serial_monitor {

class Host {
public:
    virtual ~Host() = default;

    /// Log @p message at @p level (LOGSQUIRL_LOG_*).
    virtual void log( int level, const QString& message ) = 0;

    /// Show @p message to the user as a notification.
    virtual void notify( const QString& message ) = 0;

    /// Something the user must know about: log @p message at @p level and
    /// notify it, once each, with the same text.
    void warn( const QString& message, int level = LOGSQUIRL_LOG_WARNING )
    {
        log( level, message );
        notify( message );
    }

    /// Open the file at @p path (absolute) in a new tab that follows it.
    virtual void openFollowed( const QString& path ) = 0;

    /// The plugin's configuration directory; empty if the host gives none.
    virtual QString configDir() const = 0;

    /// The directory the temporary directories are created in (tempdirs.h).
    virtual QString tempRoot() const = 0;

protected:
    Host() = default;
    Host( const Host& ) = default;
    Host& operator=( const Host& ) = default;
};

/**
 * The Host of the C ABI: forwards to the host's table @p api with @p handle.
 * Without a table (before init(), after shutdown()) it logs, notifies and
 * opens nothing, and its configuration directory is the system's temporary
 * one.
 */
class CApiHost final : public Host {
public:
    CApiHost() = default;

    /// @p tempRoot empty: the system's temporary directory.
    CApiHost( const LogSquirlHostApi* api, void* handle, const QString& tempRoot = {} );

    void log( int level, const QString& message ) override;
    void notify( const QString& message ) override;
    void openFollowed( const QString& path ) override;
    QString configDir() const override;
    QString tempRoot() const override;

    /// Whether it has a table to forward to.
    bool isAttached() const
    {
        return api_ && handle_;
    }

private:
    const LogSquirlHostApi* api_ = nullptr;
    void* handle_ = nullptr;
    QString tempRoot_;
};

/// Log, through @p host, that work for the host (@p what) failed, for @p reason.
void logFailure( Host& host, const char* what, const char* reason ) noexcept;

/// Run work for the host, logging through @p host instead of throwing: the
/// host is C, and an exception escaping into it would terminate LogSquirl.
/// Every slot the host's event loop runs goes through it.  Returns false if
/// it threw.
template <typename Work>
bool guarded( Host& host, const char* what, Work&& work ) noexcept
{
    try {
        work();
        return true;
    } catch ( const std::exception& e ) {
        logFailure( host, what, e.what() );
    } catch ( ... ) {
        logFailure( host, what, "unknown exception" );
    }
    return false;
}

} // namespace serial_monitor
