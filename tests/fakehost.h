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
 * @file fakehost.h
 * @brief In-process stand-in for the LogSquirl host API, for tests.
 *
 * A FakeHost is a plain Host (host.h): hand it to the parts under test.  It
 * has a private, empty config directory (so tests never read or write a
 * real serial.ini) and a private temporary root, and records every log
 * message, notification and tab opened for the test to inspect.
 *
 * It is the host's C table too (api(), with itself as the handle), for
 * the tests of the plugin's entry points: what the plugin does through the
 * table is recorded the same way, and so is every menu entry and sidebar
 * tab.  It installs itself nowhere.
 */

#pragma once

#include "host.h"
#include "logsquirl_plugin_api.h"

#include <catch2/catch.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <stdexcept>

namespace serial_test {

class FakeHost : public serial_monitor::Host {
public:
    /** A menu entry the plugin registered. */
    struct MenuAction {
        QString label;
        void ( *callback )( void* userData );
        void* userData;

        /** Click the entry. */
        void trigger() const
        {
            callback( userData );
        }
    };

    FakeHost()
        : configDirUtf8_( configDir_.path().toUtf8() )
    {
        api_.api_version = LOGSQUIRL_PLUGIN_API_VERSION;
        api_.log_message = []( void* handle, int level, const char* message ) {
            self( handle )->log( level, QString::fromUtf8( message ) );
        };
        api_.get_config_dir = []( void* handle ) -> const char* {
            auto* host = self( handle );
            return host->hasConfigDir ? host->configDirUtf8_.constData() : nullptr;
        };
        api_.show_notification = []( void* handle, const char* message ) {
            self( handle )->notify( QString::fromUtf8( message ) );
        };
        api_.open_file = []( void* handle, const char* filePath, int follow ) {
            self( handle )->open( QString::fromUtf8( filePath ), follow != 0 );
        };
        api_.register_menu_action = []( void* handle, const char*, const char* label,
                                        void ( *callback )( void* ), void* userData ) {
            self( handle )->menuActions.append(
                { QString::fromUtf8( label ), callback, userData } );
        };
        api_.register_sidebar_tab = []( void* handle, const char*, void* widget ) {
            auto* host = self( handle );
            if ( host->failSidebarTab ) {
                throw std::runtime_error( "no room for another tab" );
            }
            host->sidebarTabs << widget;
        };
        api_.unregister_sidebar_tab = []( void* handle, void* widget ) {
            auto* host = self( handle );
            if ( host->onUnregisterSidebarTab ) {
                host->onUnregisterSidebarTab( widget );
            }
            host->sidebarTabs.removeAll( widget );
        };
    }

    FakeHost( const FakeHost& ) = delete;
    FakeHost& operator=( const FakeHost& ) = delete;

    // ── Host ─────────────────────────────────────────────────────────

    void log( int level, const QString& message ) override
    {
        logs << message;
        logLevels << level;
        if ( !failLogContaining.isEmpty() && message.contains( failLogContaining ) ) {
            throw std::runtime_error( "the log is full" );
        }
    }

    void notify( const QString& message ) override
    {
        notifications << message;
    }

    void openFollowed( const QString& path ) override
    {
        open( path, true );
    }

    /** The plugin's config directory (empty until a test writes to it). */
    QString configDir() const override
    {
        return hasConfigDir ? configDir_.path() : QString();
    }

    /** The plugin's temporary root, private to this host. */
    QString tempRoot() const override
    {
        return tempRoot_.path();
    }

    // ── The C table ──────────────────────────────────────────────────

    /** The host API table, to pass to logsquirl_plugin_init(). */
    const LogSquirlHostApi* api() const
    {
        return &api_;
    }

    /** The handle to pass to logsquirl_plugin_init() with api(). */
    void* handle()
    {
        return this;
    }

    // ── What was recorded ────────────────────────────────────────────

    QStringList logs;
    QList<int> logLevels; ///< The level of each of logs.
    QStringList notifications;
    QList<void*> sidebarTabs; ///< The widgets registered as sidebar tabs.
    /// register_sidebar_tab() throws, as a host out of room might.
    bool failSidebarTab = false;
    /// Called by unregister_sidebar_tab() with the widget, before the tab
    /// goes: to look at the plugin then, or to throw.
    std::function<void( void* widget )> onUnregisterSidebarTab;
    /// configDir() and get_config_dir() give the config directory; empty
    /// (NULL), as a host without one does, when false.
    bool hasConfigDir = true;
    /// log() and log_message() throw after logging a message containing this.
    QString failLogContaining;
    QStringList openedFiles;
    QStringList followedFiles; ///< Those of openedFiles opened in follow mode.
    QList<MenuAction> menuActions;

private:
    static FakeHost* self( void* handle )
    {
        return static_cast<FakeHost*>( handle );
    }

    void open( const QString& path, bool follow )
    {
        openedFiles << path;
        if ( follow ) {
            followedFiles << path;
        }
    }

    QTemporaryDir configDir_;
    QTemporaryDir tempRoot_;
    QByteArray configDirUtf8_;
    LogSquirlHostApi api_{};
};

/** Run the event loop until @p condition holds or @p timeoutMs passes. */
inline bool waitFor( const std::function<bool()>& condition, int timeoutMs = 5000 )
{
    QElapsedTimer timer;
    timer.start();
    while ( !condition() ) {
        if ( timer.elapsed() > timeoutMs ) {
            return false;
        }
        QCoreApplication::processEvents();
        QThread::msleep( 5 );
    }
    return true;
}

/** Run the event loop for @p ms milliseconds. */
inline void processEventsFor( int ms )
{
    waitFor( []() { return false; }, ms );
}

} // namespace serial_test

// Let Catch print Qt strings in failure messages.
namespace Catch {
template <>
struct StringMaker<QString> {
    static std::string convert( const QString& value )
    {
        return '"' + value.toStdString() + '"';
    }
};

template <>
struct StringMaker<QByteArray> {
    static std::string convert( const QByteArray& value )
    {
        return '"' + value.toStdString() + '"';
    }
};
} // namespace Catch
