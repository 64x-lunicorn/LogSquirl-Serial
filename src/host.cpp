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
 * @file host.cpp
 * @brief CApiHost, the Host of the C ABI, and logFailure().
 */

#include "host.h"

#include <QDir>
#include <QStandardPaths>

namespace serial_monitor {

CApiHost::CApiHost( const LogSquirlHostApi* api, void* handle, const QString& tempRoot )
    : api_( api )
    , handle_( handle )
    , tempRoot_( tempRoot )
{
}

// The host decodes every string as UTF-8.

void CApiHost::log( int level, const QString& message )
{
    if ( isAttached() ) {
        api_->log_message( handle_, level, message.toUtf8().constData() );
    }
}

void CApiHost::notify( const QString& message )
{
    if ( isAttached() ) {
        api_->show_notification( handle_, message.toUtf8().constData() );
    }
}

void CApiHost::openFollowed( const QString& path )
{
    if ( isAttached() ) {
        api_->open_file( handle_, path.toUtf8().constData(), 1 );
    }
}

QString CApiHost::configDir() const
{
    if ( isAttached() ) {
        return QString::fromUtf8( api_->get_config_dir( handle_ ) );
    }
    return QStandardPaths::writableLocation( QStandardPaths::TempLocation );
}

QString CApiHost::tempRoot() const
{
    return tempRoot_.isEmpty() ? QDir::tempPath() : tempRoot_;
}

void logFailure( Host& host, const char* what, const char* reason ) noexcept
{
    try {
        host.log( LOGSQUIRL_LOG_ERROR, QStringLiteral( "Serial Monitor plugin: %1 failed: %2" )
                                           .arg( what, QString::fromUtf8( reason ) ) );
    } catch ( ... ) {
        // Nothing left to report it with.
    }
}

} // namespace serial_monitor
