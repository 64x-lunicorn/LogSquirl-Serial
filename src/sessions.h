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
 * @file sessions.h
 * @brief The running captures, one per port, and the list of ports.
 *
 * Sessions starts a SerialProcess for a port, opens its log file in a
 * followed tab, opens each next file in a new tab when the log is rotated
 * (by the user or at its size limit), and lets go of the session when it is
 * stopped or ends on its own (device unplugged, log file not writable).  A
 * port runs one session at a time; several ports run side by side, each
 * with its own file and tab.  It also keeps the result of the last port
 * scan, which every view shares.
 *
 * The sidebar (SidebarWidget) and its tiles are views of it: a tile starts
 * the session of its saved configuration (profiles.h), named as the
 * session's owner, and finds it again through portOf().  The views call
 * its methods and update on its signals.  Starting,
 * stopping and errors are notifications of the Host it is handed (host.h),
 * sent from here; a refusal is a Host::warn(), logged and notified; stopAll()
 * notifies nothing (the views say "All serial sessions stopped.", the
 * shutdown says nothing).
 *
 * A running session's counters (lines, bytes, file size, rotations, …) are
 * read from session(); they change on every line, so no signal says so.
 *
 * OWNERSHIP
 * ─────────
 * Created by logsquirl_plugin_init(), deleted by logsquirl_plugin_shutdown()
 * after stopAll() and after the views.  The running SerialProcesses are its
 * children: deleting it with sessions still running stops them without a
 * signal, and removes the temporary files of those not preserved.
 */

#pragma once

#include "serialprocess.h"

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

namespace serial_monitor {

class Sessions : public QObject {
    Q_OBJECT

public:
    /**
     * Scans the ports once (ports()), without emitting portsChanged().
     * Everything it and its sessions tell LogSquirl goes through @p host,
     * which must outlive it.
     */
    explicit Sessions( Host& host, QObject* parent = nullptr );
    ~Sessions() override = default;

    /**
     * Start capture for config.portName and open its log file in a
     * followed tab.  Refused, with a host notification saying why, if the
     * port has a session already (naming the configuration that started
     * it), the baud rate is not one, the port cannot be opened, or another
     * session writes to @p savePath.  sessionStarted() follows a session
     * that started.
     *
     * @param savePath  Optional path to a .log file for persistent saving.
     * @param owner     The saved configuration that starts it (profiles.h),
     *                  if any: ownerOf() and portOf() tell it until it ends.
     * @return Whether the session started.
     */
    bool startSession( const SerialConfig& config, const QString& savePath = {},
                       const QString& owner = {} );

    /**
     * Stop the session of @p portName, keep its file for its tab, and say
     * so in a host notification; sessionStopped() follows.  No-op if it has
     * none.
     */
    void stopSession( const QString& portName );

    /**
     * Stop every session, keeping their files for their tabs, also when
     * the plugin is disabled or updated at runtime (the plugin removes the
     * temporary ones when LogSquirl quits, see tempdirs.h).
     * sessionStopped() follows for each; no notification is sent.
     */
    void stopAll();

    /**
     * Rotate the log of @p portName's session: the old file stays for its
     * tab, and the capture goes on in a new file, opened in a new tab;
     * sessionChanged() follows.  A failure is reported by a notification,
     * and the session goes on in the old file.  No-op without a running
     * session.
     */
    void rotateSession( const QString& portName );

    /**
     * Send @p data with @p lineEnding to @p portName's session.
     *
     * @return Whether it was sent; false without a running session.
     */
    bool sendToSession( const QString& portName, const QByteArray& data, TxLineEnding lineEnding );

    /** Whether @p portName has a session. */
    bool isSessionActive( const QString& portName ) const;

    /** Number of sessions. */
    int activeSessionCount() const;

    /** The ports that have a session, sorted. */
    QStringList activePorts() const;

    /**
     * The session of @p portName, to read its counters and settings, or
     * nullptr if it has none.  Valid until sessionStopped( portName ).
     */
    const SerialProcess* session( const QString& portName ) const;

    /**
     * The saved configuration whose session runs on @p portName (the owner
     * given to startSession()); empty if the port has no session, or one
     * started without a configuration.
     */
    QString ownerOf( const QString& portName ) const;

    /**
     * The port of the session that the saved configuration @p owner
     * started; empty if it runs none.  A tile finds its session this way,
     * not through the port saved in its configuration, which may have been
     * edited since.
     */
    QString portOf( const QString& owner ) const;

    /** Names of the ports found by the most recent scan. */
    const QStringList& ports() const
    {
        return ports_;
    }

public Q_SLOTS:
    /**
     * Re-scan for serial ports.  The enumeration blocks, so it only runs
     * when asked for (Refresh, a session ending on its own) and at
     * construction, not on every start and stop; portsChanged() is emitted
     * when ports() is updated.
     */
    void refreshPorts();

Q_SIGNALS:
    /** A session for @p portName started; its tab is open. */
    void sessionStarted( const QString& portName );

    /**
     * The session for @p portName ended: stopped, or on its own.  It no
     * longer counts as active, and session( portName ) is nullptr.
     */
    void sessionStopped( const QString& portName );

    /** The session for @p portName goes on in a new log file (rotated). */
    void sessionChanged( const QString& portName );

    /** A port scan has finished and ports() is updated. */
    void portsChanged();

private:
    /** Rotate @p portName's log and open the new file; tell the user @p notification. */
    void rotateLogOf( const QString& portName, SerialProcess::PartialLine partialLine,
                      const QString& notification );

    /** A session ended on its own: let go of it. */
    void onSessionFinished( const QString& portName );

    /** A session reported an error: log it and tell the user. */
    void onSessionError( const QString& portName, const QString& message );

    /** Rotate a session whose log file reached its size limit. */
    void onSizeLimitReached( const QString& portName );

    /**
     * Remove the session for @p portName from the active sessions and cut
     * its signals to this object.  The caller stops and deletes it, and
     * emits sessionStopped().
     *
     * @return The session, or nullptr if there is none for @p portName.
     */
    SerialProcess* takeSession( const QString& portName );

    /** Whether an active session writes to the file at @p path. */
    bool isFileInUse( const QString& path ) const;

    /** Scan the ports, and log how many there are. */
    QStringList scanPorts();

    Host& host_;
    QStringList ports_; ///< Result of the last port scan.
    QMap<QString, SerialProcess*> sessions_;
    QMap<QString, QString> owners_; ///< Port → the configuration that started its session.
};

} // namespace serial_monitor
