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
 * @file profiles.cpp
 * @brief Implementation of the saved port configurations and their store.
 */

#include "profiles.h"

#include <QDir>
#include <QSettings>

#include <algorithm>
#include <array>
#include <utility>

namespace serial_monitor {

namespace {

constexpr const char* kProfiles = "profiles";

/// The log directory saved last: where the sidebar of earlier versions
/// saved its one log directory, so a new configuration proposes it.
constexpr const char* kLogDirKey = "serial/logDir";

/// A setting's values as saved, and as a terminal names them.
template <typename Value>
struct Named {
    Value value;
    const char* saved;
    const char* shown;
};

const std::array<Named<QSerialPort::DataBits>, 4> kDataBits{ {
    { QSerialPort::Data5, "5", "5" },
    { QSerialPort::Data6, "6", "6" },
    { QSerialPort::Data7, "7", "7" },
    { QSerialPort::Data8, "8", "8" },
} };

const std::array<Named<QSerialPort::StopBits>, 3> kStopBits{ {
    { QSerialPort::OneStop, "1", "1" },
    { QSerialPort::OneAndHalfStop, "1.5", "1.5" },
    { QSerialPort::TwoStop, "2", "2" },
} };

const std::array<Named<QSerialPort::Parity>, 5> kParities{ {
    { QSerialPort::NoParity, "none", "N" },
    { QSerialPort::EvenParity, "even", "E" },
    { QSerialPort::OddParity, "odd", "O" },
    { QSerialPort::MarkParity, "mark", "M" },
    { QSerialPort::SpaceParity, "space", "S" },
} };

const std::array<Named<QSerialPort::FlowControl>, 3> kFlowControls{ {
    { QSerialPort::NoFlowControl, "none", "no flow control" },
    { QSerialPort::HardwareControl, "rtscts", "RTS/CTS" },
    { QSerialPort::SoftwareControl, "xonxoff", "XON/XOFF" },
} };

/// The entry of @p table for @p value; the first if there is none.
template <typename Value, std::size_t N>
const Named<Value>& entryFor( const std::array<Named<Value>, N>& table, Value value )
{
    const auto it = std::find_if( table.begin(), table.end(),
                                  [ value ]( const auto& entry ) { return entry.value == value; } );
    return it != table.end() ? *it : table.front();
}

/// The value saved as @p saved in @p table; @p fallback if none is.
template <typename Value, std::size_t N>
Value valueFor( const std::array<Named<Value>, N>& table, const QString& saved, Value fallback )
{
    for ( const auto& entry : table ) {
        if ( saved == QLatin1String( entry.saved ) ) {
            return entry.value;
        }
    }
    return fallback;
}

} // namespace

QString defaultLogDir()
{
    const auto cfgDir = SerialProcess::configDir();
    return cfgDir.isEmpty() ? QString() : cfgDir + "/logs";
}

QString frameOf( const SerialConfig& config )
{
    return QStringLiteral( "%1 %2%3%4" )
        .arg( config.baudRate )
        .arg( QLatin1String( entryFor( kDataBits, config.dataBits ).shown ),
              QLatin1String( entryFor( kParities, config.parity ).shown ),
              QLatin1String( entryFor( kStopBits, config.stopBits ).shown ) );
}

QString flowControlName( QSerialPort::FlowControl flowControl )
{
    return QLatin1String( entryFor( kFlowControls, flowControl ).shown );
}

QString startProblem( const PortProfile& profile )
{
    if ( profile.serial.portName.trimmed().isEmpty() ) {
        return QStringLiteral( "Choose the port." );
    }
    if ( profile.serial.baudRate <= 0 ) {
        return QStringLiteral( "Enter a baud rate, such as 115200." );
    }
    if ( !profile.logDir.trimmed().isEmpty()
         && SerialProcess::absoluteUserPath( profile.logDir ).isEmpty() ) {
        return QStringLiteral( "The log directory %1 is not a full path; give one such as %2." )
            .arg( profile.logDir, QDir::toNativeSeparators( QDir::homePath() + "/serial-logs" ) );
    }
    return {};
}

SerialConfig sessionConfig( const PortProfile& profile )
{
    auto config = profile.serial;
    config.maxFileBytes = SerialProcess::defaultConfig().maxFileBytes;
    return config;
}

QString savePathFor( const PortProfile& profile )
{
    auto dir = profile.logDir.trimmed();
    if ( dir.isEmpty() ) {
        dir = defaultLogDir();
        if ( dir.isEmpty() ) {
            return {};
        }
    }

    // A relative directory is left as it is: SerialProcess::start() refuses
    // it with a message, rather than create it in LogSquirl's working
    // directory.
    if ( const auto absolute = SerialProcess::absoluteUserPath( dir ); !absolute.isEmpty() ) {
        dir = absolute;
        QDir().mkpath( dir );
    }
    return SerialProcess::generateLogPath( dir, profile.serial.portName );
}

// ── The store ───────────────────────────────────────────────────────────

ProfileStore::ProfileStore( const QString& settingsFile, IsRunning isRunning, QObject* parent )
    : QObject( parent )
    , settingsFile_( settingsFile )
    , isRunning_( std::move( isRunning ) )
{
}

QString ProfileStore::settingsFile() const
{
    return settingsFile_;
}

QList<PortProfile> ProfileStore::list() const
{
    QList<PortProfile> profiles;
    if ( settingsFile_.isEmpty() ) {
        return profiles;
    }
    const SerialConfig defaults;
    QSettings settings( settingsFile_, QSettings::IniFormat );
    const int count = settings.beginReadArray( kProfiles );
    for ( int i = 0; i < count; ++i ) {
        settings.setArrayIndex( i );
        PortProfile profile;
        profile.name = settings.value( "name" ).toString();
        if ( profile.name.isEmpty() ) {
            continue;
        }
        auto& serial = profile.serial;
        serial.portName = settings.value( "port" ).toString();
        bool ok = false;
        const int baudRate = settings.value( "baudRate" ).toInt( &ok );
        serial.baudRate = ok && baudRate > 0 ? baudRate : defaults.baudRate;
        serial.dataBits
            = valueFor( kDataBits, settings.value( "dataBits" ).toString(), defaults.dataBits );
        serial.stopBits
            = valueFor( kStopBits, settings.value( "stopBits" ).toString(), defaults.stopBits );
        serial.parity
            = valueFor( kParities, settings.value( "parity" ).toString(), defaults.parity );
        serial.flowControl = valueFor( kFlowControls, settings.value( "flowControl" ).toString(),
                                       defaults.flowControl );
        serial.timestamps = settings.value( "timestamps", defaults.timestamps ).toBool();
        profile.logDir = settings.value( "logDir" ).toString();
        profiles << profile;
    }
    settings.endArray();
    return profiles;
}

std::optional<PortProfile> ProfileStore::find( const QString& name ) const
{
    for ( const auto& profile : list() ) {
        if ( profile.name == name ) {
            return profile;
        }
    }
    return std::nullopt;
}

PortProfile ProfileStore::newProfile() const
{
    PortProfile profile;
    const auto defaults = SerialProcess::defaultConfig();
    profile.serial.baudRate = defaults.baudRate;
    profile.serial.timestamps = defaults.timestamps;
    if ( !settingsFile_.isEmpty() ) {
        const QSettings settings( settingsFile_, QSettings::IniFormat );
        profile.logDir = settings.value( kLogDirKey ).toString();
    }
    return profile;
}

QString ProfileStore::upsert( const PortProfile& profile )
{
    if ( profile.name.isEmpty() ) {
        return QStringLiteral( "Enter a name for the configuration to save." );
    }
    if ( settingsFile_.isEmpty() ) {
        return QStringLiteral( "The configuration %1 cannot be saved: LogSquirl gave the plugin "
                               "no configuration directory to save it in." )
            .arg( profile.name );
    }
    auto profiles = list();
    const auto existing = std::find_if( profiles.begin(), profiles.end(),
                                        [ & ]( const auto& p ) { return p.name == profile.name; } );
    if ( existing != profiles.end() ) {
        *existing = profile;
    }
    else {
        profiles << profile;
    }
    if ( !write( profiles, profile.logDir ) ) {
        return QStringLiteral( "The configuration %1 could not be saved to %2." )
            .arg( profile.name, QDir::toNativeSeparators( settingsFile_ ) );
    }
    Q_EMIT changed();
    return {};
}

QString ProfileStore::remove( const QString& name )
{
    if ( isRunning_ && isRunning_( name ) ) {
        return QStringLiteral( "The configuration %1 is capturing: stop it to delete it." )
            .arg( name );
    }
    auto profiles = list();
    if ( profiles.removeIf( [ & ]( const auto& p ) { return p.name == name; } ) == 0 ) {
        return QStringLiteral( "There is no saved configuration %1." ).arg( name );
    }
    if ( !write( profiles, std::nullopt ) ) {
        return QStringLiteral( "The configuration %1 could not be deleted from %2." )
            .arg( name, QDir::toNativeSeparators( settingsFile_ ) );
    }
    Q_EMIT changed();
    return {};
}

bool ProfileStore::write( const QList<PortProfile>& profiles, const std::optional<QString>& logDir )
{
    if ( settingsFile_.isEmpty() ) {
        return false;
    }
    QSettings settings( settingsFile_, QSettings::IniFormat );
    if ( logDir ) {
        settings.setValue( kLogDirKey, logDir->trimmed() );
    }
    settings.remove( kProfiles );
    settings.beginWriteArray( kProfiles, static_cast<int>( profiles.size() ) );
    for ( int i = 0; i < profiles.size(); ++i ) {
        const auto& profile = profiles.at( i );
        const auto& serial = profile.serial;
        settings.setArrayIndex( i );
        settings.setValue( "name", profile.name );
        settings.setValue( "port", serial.portName );
        settings.setValue( "baudRate", serial.baudRate );
        settings.setValue( "dataBits",
                           QLatin1String( entryFor( kDataBits, serial.dataBits ).saved ) );
        settings.setValue( "stopBits",
                           QLatin1String( entryFor( kStopBits, serial.stopBits ).saved ) );
        settings.setValue( "parity", QLatin1String( entryFor( kParities, serial.parity ).saved ) );
        settings.setValue( "flowControl",
                           QLatin1String( entryFor( kFlowControls, serial.flowControl ).saved ) );
        settings.setValue( "timestamps", serial.timestamps );
        settings.setValue( "logDir", profile.logDir.trimmed() );
    }
    settings.endArray();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

} // namespace serial_monitor
