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
 * @file sessions.cpp
 * @brief Implementation of Sessions, the running captures.
 *
 * Sessions keeps a map of portName → SerialProcess*.  Starting a session
 * from a saved configuration (start()):
 *
 *   0. The configuration is checked (startProblem()), the session's settings
 *      taken from it and the defaults, and its log file picked (logPathFor());
 *      the port must be free, and the file not another session's.
 *   1. A SerialProcess is created with the given configuration.
 *   2. SerialProcess::start() creates the log directory, and opens the log
 *      file and the serial port.
 *   3. Host::openFollowed() is called with the log file's path, which opens
 *      a new tail-mode tab in LogSquirl.
 *   4. The session is stored in sessions_, and sessionStarted() emitted.
 *
 * Stopping one:
 *   1. It is taken out of sessions_, and its signals to Sessions cut.
 *   2. SerialProcess::stop() closes the serial port; the file is kept.
 *   3. sessionStopped() is emitted.  The LogSquirl tab remains open.
 */

#include "sessions.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>

namespace serial_monitor {

namespace {

/**
 * Whether @p a and @p b name the same file.  Existing files are compared
 * by their canonical path (resolving symlinks); otherwise the cleaned
 * absolute paths are compared.  QFileInfo's own operator== cannot be
 * used: two files that do not exist both have an empty canonical path,
 * and compare equal.
 */
bool isSameFile( const QString& a, const QString& b )
{
    const QFileInfo fileA( a );
    const QFileInfo fileB( b );
    if ( fileA.exists() && fileB.exists() ) {
        return fileA.canonicalFilePath() == fileB.canonicalFilePath();
    }
#if defined( Q_OS_WIN ) || defined( Q_OS_MACOS )
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath( fileA.absoluteFilePath() )
               .compare( QDir::cleanPath( fileB.absoluteFilePath() ), sensitivity )
           == 0;
}

/// What the user is notified of an error @p message of @p portName's session.
QString errorNotification( const QString& portName, const QString& message )
{
    return "Serial error (" + portName + "): " + message;
}

/**
 * A new log file for a capture of @p profile, in its log directory, or in
 * @p defaultLogDir when it has none; empty when both are, for a temporary
 * file.  The directory is created where the file is opened
 * (SerialProcess::start()), once the port is known to be free.
 */
QString logPathFor( const PortProfile& profile, const QString& defaultLogDir )
{
    auto dir = profile.logDir.trimmed();
    if ( dir.isEmpty() ) {
        dir = defaultLogDir;
    }
    if ( dir.isEmpty() ) {
        return {};
    }
    // startProblem() has refused a log directory that is not a full path; a
    // default one that is not is left for SerialProcess::start() to refuse
    // with a message, rather than be written below LogSquirl's working
    // directory.
    const auto absolute = SerialProcess::absoluteUserPath( dir );
    return SerialProcess::generateLogPath( absolute.isEmpty() ? dir : absolute,
                                           profile.serial.portName );
}

} // namespace

Sessions::Sessions( Host& host, QObject* parent )
    : QObject( parent )
    , host_( host )
    , ports_( scanPorts() )
{
}

QStringList Sessions::scanPorts()
{
    auto ports = SerialProcess::discoverPorts();
    host_.log( LOGSQUIRL_LOG_INFO, QString( "Discovered %1 serial port(s)." ).arg( ports.size() ) );
    return ports;
}

// ── Starting and stopping ───────────────────────────────────────────────

QString Sessions::startProblem( const PortProfile& profile )
{
    if ( profile.serial.portName.trimmed().isEmpty() ) {
        return QStringLiteral( "Choose the port." );
    }
    // The baud rate combo is editable, so its text may not be a rate.
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

QString Sessions::start( const PortProfile& profile, const Defaults& defaults,
                         const QString& defaultLogDir )
{
    if ( const auto problem = startProblem( profile ); !problem.isEmpty() ) {
        return refuse(
            QStringLiteral( "Serial capture %1 not started: %2" ).arg( profile.name, problem ) );
    }
    auto config = profile.serial;
    config.maxFileBytes = defaults.maxFileBytes();
    return launch( config, logPathFor( profile, defaultLogDir ), profile.name );
}

bool Sessions::startSession( const SerialConfig& config, const QString& savePath,
                             const QString& owner )
{
    return launch( config, savePath, owner ).isEmpty();
}

QString Sessions::refuse( const QString& message )
{
    host_.warn( message );
    return message;
}

QString Sessions::launch( const SerialConfig& config, const QString& savePath,
                          const QString& owner )
{
    const auto& name = config.portName;
    if ( name.isEmpty() ) {
        return refuse( QStringLiteral( "Serial capture not started: no port." ) );
    }

    // One session a port: the port would be opened twice.
    if ( sessions_.contains( name ) ) {
        const auto holder = owners_.value( name );
        return refuse(
            holder.isEmpty()
                ? QString( "Serial capture not started for %1: the port is already being "
                           "captured." )
                      .arg( name )
                : QString( "Serial capture not started for %1: the port is already being "
                           "captured by “%2”; stop it first." )
                      .arg( name, holder ) );
    }

    // Two sessions appending to one file would interleave their lines.
    if ( !savePath.isEmpty() && isFileInUse( savePath ) ) {
        return refuse( QString( "Serial capture not started for %1: another session is "
                                "already writing to %2." )
                           .arg( name, savePath ) );
    }

    auto* proc = new SerialProcess( host_, config, savePath, this );

    // What the session reports comes in the host's event loop: nothing may
    // throw into it.
    connect( proc, &SerialProcess::finished, this, [ this, name ] {
        guarded( host_, "ending a serial capture", [ this, &name ] { onSessionFinished( name ); } );
    } );

    connect( proc, &SerialProcess::errorOccurred, this, [ this, name ]( const QString& msg ) {
        guarded( host_, "reporting a serial capture's error",
                 [ this, &name, &msg ] { onSessionError( name, msg ); } );
    } );

    connect( proc, &SerialProcess::sizeLimitReached, this, [ this, name ] {
        guarded( host_, "starting a new log file at a serial capture's size limit",
                 [ this, &name ] { onSizeLimitReached( name ); } );
    } );

    // start() reports why it fails through errorOccurred(), which
    // onSessionError() has told the user; what it told is the problem.
    QString failure;
    const auto recording = connect( proc, &SerialProcess::errorOccurred, this,
                                    [ &failure, name ]( const QString& message ) {
                                        failure = errorNotification( name, message );
                                    } );
    const bool started = proc->start();
    disconnect( recording );
    if ( !started ) {
        delete proc;
        return failure.isEmpty()
                   ? refuse( QString( "Serial capture not started for %1." ).arg( name ) )
                   : failure;
    }

    sessions_.insert( name, proc );
    if ( !owner.isEmpty() ) {
        owners_.insert( name, owner );
    }

    host_.openFollowed( proc->tempFilePath() );
    host_.notify(
        QString( "Serial capture started for %1 at %2 baud" ).arg( name ).arg( config.baudRate ) );

    Q_EMIT sessionStarted( name );
    return {};
}

void Sessions::stopSession( const QString& portName )
{
    auto* proc = takeSession( portName );
    if ( !proc ) {
        return;
    }

    proc->stop();
    proc->preserveTempFile();

    host_.notify( QString( "Serial capture stopped for %1 (%2 lines)" )
                      .arg( portName )
                      .arg( proc->lineCount() ) );

    proc->deleteLater();
    Q_EMIT sessionStopped( portName );
}

void Sessions::stopAll()
{
    const auto portNames = sessions_.keys();
    for ( const auto& name : portNames ) {
        auto* proc = takeSession( name );
        proc->stop();
        proc->preserveTempFile();
        proc->deleteLater();
        Q_EMIT sessionStopped( name );
    }
}

void Sessions::rotateSession( const QString& portName )
{
    rotateLogOf( portName, SerialProcess::PartialLine::WriteOut,
                 QString( "New session started for %1" ).arg( portName ) );
}

void Sessions::rotateLogOf( const QString& portName, SerialProcess::PartialLine partialLine,
                            const QString& notification )
{
    auto* proc = sessions_.value( portName, nullptr );
    if ( !proc || !proc->isRunning() ) {
        return;
    }

    const auto newPath = proc->rotateLog( partialLine );
    if ( newPath.isEmpty() ) {
        // rotateLog() has reported why through errorOccurred()
        return;
    }

    // The old tab keeps showing the old file, so the temporary directory
    // must outlive this session (until LogSquirl quits, see tempdirs.h).
    proc->preserveTempFile();

    host_.openFollowed( newPath );
    host_.notify( notification );

    Q_EMIT sessionChanged( portName );
}

bool Sessions::sendToSession( const QString& portName, const QByteArray& data,
                              TxLineEnding lineEnding )
{
    auto* proc = sessions_.value( portName, nullptr );
    if ( !proc || !proc->isRunning() ) {
        return false;
    }

    return proc->sendData( data, lineEnding );
}

// ── Queries ─────────────────────────────────────────────────────────────

bool Sessions::isSessionActive( const QString& portName ) const
{
    return sessions_.contains( portName );
}

int Sessions::activeSessionCount() const
{
    return static_cast<int>( sessions_.size() );
}

QStringList Sessions::activePorts() const
{
    return sessions_.keys();
}

const SerialProcess* Sessions::session( const QString& portName ) const
{
    return sessions_.value( portName, nullptr );
}

// ── By configuration ────────────────────────────────────────────────────

bool Sessions::isRunning( const QString& name ) const
{
    return !portOf( name ).isEmpty();
}

ProfileStore::IsRunning Sessions::runningCheck() const
{
    return [ this ]( const QString& name ) { return isRunning( name ); };
}

void Sessions::stop( const QString& name )
{
    if ( const auto port = portOf( name ); !port.isEmpty() ) {
        stopSession( port );
    }
}

void Sessions::rotate( const QString& name )
{
    if ( const auto port = portOf( name ); !port.isEmpty() ) {
        rotateSession( port );
    }
}

bool Sessions::send( const QString& name, const QByteArray& data, TxLineEnding lineEnding )
{
    const auto port = portOf( name );
    return !port.isEmpty() && sendToSession( port, data, lineEnding );
}

const SerialProcess* Sessions::captureOf( const QString& name ) const
{
    const auto port = portOf( name );
    return port.isEmpty() ? nullptr : session( port );
}

QString Sessions::portInUse( const QString& portName ) const
{
    if ( !sessions_.contains( portName ) ) {
        return {};
    }
    const auto owner = owners_.value( portName );
    return owner.isEmpty() ? QString( "%1 is in use by another capture" ).arg( portName )
                           : QString( "%1 is in use by “%2”" ).arg( portName, owner );
}

QString Sessions::portOf( const QString& name ) const
{
    return name.isEmpty() ? QString() : owners_.key( name );
}

void Sessions::refreshPorts()
{
    ports_ = scanPorts();
    Q_EMIT portsChanged();
}

// ── What sessions report ────────────────────────────────────────────────

void Sessions::onSessionFinished( const QString& portName )
{
    auto* proc = takeSession( portName );
    if ( !proc ) {
        return;
    }

    // Preserve the temp file so the LogSquirl tab keeps its content.
    // When using a save path the file is already persistent.
    proc->preserveTempFile();
    proc->deleteLater();

    host_.log( LOGSQUIRL_LOG_INFO, QString( "Serial session for %1 ended." ).arg( portName ) );
    Q_EMIT sessionStopped( portName );

    // A session ends by itself when its device is unplugged; find out
    // whether the port is gone
    refreshPorts();
}

void Sessions::onSizeLimitReached( const QString& portName )
{
    const auto* proc = sessions_.value( portName, nullptr );
    if ( !proc ) {
        return;
    }
    // The line being received goes on in the new file, kept whole.
    rotateLogOf(
        portName, SerialProcess::PartialLine::Keep,
        QString( "The capture of %1 reached its size limit of %2; it goes on in a new tab." )
            .arg( portName, QLocale().formattedDataSize( proc->config().maxFileBytes ) ) );
}

void Sessions::onSessionError( const QString& portName, const QString& message )
{
    // Not a warn(): the log names the port the way the other log lines do,
    // the notification says what kind of message it is.
    host_.log( LOGSQUIRL_LOG_ERROR, portName + ": " + message );
    host_.notify( errorNotification( portName, message ) );
}

// ── Private helpers ─────────────────────────────────────────────────────

SerialProcess* Sessions::takeSession( const QString& portName )
{
    auto* proc = sessions_.take( portName );
    owners_.remove( portName );
    if ( proc ) {
        // The session is over as far as Sessions is concerned.  Stopping it
        // emits finished(), and onSessionFinished() must not act on that:
        // it would end the session a second time, and rescan the ports once
        // per session.
        proc->disconnect( this );
    }
    return proc;
}

bool Sessions::isFileInUse( const QString& path ) const
{
    for ( const auto* proc : sessions_ ) {
        if ( isSameFile( proc->tempFilePath(), path ) ) {
            return true;
        }
    }
    return false;
}

} // namespace serial_monitor
