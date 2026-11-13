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
 * the capture of its saved configuration (profiles.h) in one call,
 * start(), which checks the configuration, sets the capture up from it and
 * the defaults, picks its log file and refuses a busy port; the tile then
 * stops, rotates, sends to and reads it by the configuration's name
 * (stop(), rotate(), send(), captureOf()), which do nothing once it has
 * ended.  The profile store asks it whether a configuration captures
 * (runningCheck()), the dialog which ports there are (ports()).  The views
 * call its methods and update on its signals.
 * Starting, stopping and errors are notifications of the Host it is handed (host.h),
 * sent from here; a refusal is a Host::warn(), logged and notified; stopAll()
 * notifies nothing (the views say "All serial sessions stopped.", the
 * shutdown says nothing).
 *
 * A running session's counters (lines, bytes, file size, rotations, …) are
 * read from session(); they change on every line, so no signal says so.
 *
 * OWNERSHIP
 * ─────────
 * A member of the plugin instance (plugin.cpp): destroyed after stopAll() and
 * after the store and the views that use it.  The running SerialProcesses are its
 * children: deleting it with sessions still running stops them without a
 * signal, and removes the temporary files of those not preserved.
 */

#pragma once

#include "profiles.h"
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
     * What keeps @p profile from starting a capture, in words for the user:
     * no port, no baud rate, or a log directory that is not a full path;
     * empty if nothing (whether its port and its log file open shows at
     * start()).  The dialog saves no configuration that it refuses.
     */
    static QString startProblem( const PortProfile& profile );

    /**
     * Start a capture of the saved configuration @p profile, as its owner
     * (the calls by configuration name below find it by its name until it
     * ends), and open its log file in a followed tab.
     *
     * The capture has the settings of @p profile, with the size limit of
     * @p defaults (ProfileStore::defaults()).  It writes to a new file,
     * YYYY-MM-dd_HHmmss_<port>.log, in the profile's log directory, or in
     * @p defaultLogDir (ProfileStore::defaultLogDir()) when it has none,
     * created if it does not exist; it is refused when both are empty, as a
     * capture of a configuration is never a temporary file.
     *
     * Refused when startProblem() says why, when the configuration
     * captures already (on whichever port: one configuration never has two
     * captures), when the port has a capture already (naming the
     * configuration that holds it), when another capture
     * writes to the file, or when the port or the file does not open.  The
     * host has been told each refusal once: a Host::warn(), or, for a port
     * or file that does not open, the error the capture reports (logged as
     * an error, and notified).  sessionStarted() follows a capture that
     * started.
     *
     * @return Why it was refused, as the user was notified; empty if it
     *         started.
     */
    QString start( const PortProfile& profile, const Defaults& defaults,
                   const QString& defaultLogDir );

    /**
     * Start capture for config.portName with @p config as it is, and open
     * its log file in a followed tab: start() without a saved configuration
     * to check and derive it from.  Refused, as start() is, if @p owner
     * captures already, if the port has a session already, another session writes to @p savePath,
     * or the port or the file does not open.  sessionStarted() follows a session that started.
     *
     * @param savePath  Optional full path to a .log file for persistent
     *                  saving; empty for a temporary file.
     * @param owner     The saved configuration that starts it (profiles.h),
     *                  if any: the calls by configuration name below find
     *                  the session through it until it ends.
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

    // ── By configuration ────────────────────────────────────────────────
    //
    // A saved configuration (profiles.h) knows its capture by its name, the
    // owner given to startSession(), not by the port saved in it, which may
    // have been edited since.  For a name that captures nothing (never
    // started, stopped, ended on its own, or empty) each of these does
    // nothing and tells nothing.

    /** Whether the saved configuration @p name captures. */
    bool isRunning( const QString& name ) const;

    /**
     * isRunning() as the profile store asks it before a rename or delete
     * (ProfileStore::IsRunning); valid while this object lives.
     */
    ProfileStore::IsRunning runningCheck() const;

    /** Stop the capture of the configuration @p name, as stopSession(). */
    void stop( const QString& name );

    /** Rotate the log of the configuration @p name's capture, as rotateSession(). */
    void rotate( const QString& name );

    /**
     * Send @p data with @p lineEnding to the capture of the configuration
     * @p name, as sendToSession().
     *
     * @return Whether it was sent; false if @p name captures nothing.
     */
    bool send( const QString& name, const QByteArray& data, TxLineEnding lineEnding );

    /**
     * The capture of the configuration @p name, to read its counters and
     * settings, or nullptr if it runs none.  Valid until sessionStopped()
     * for its port.
     */
    const SerialProcess* captureOf( const QString& name ) const;

    /**
     * Why @p portName cannot be captured now, as a tile shows it: "<port>
     * is in use by “board”", or "… by another capture" for a session
     * started without a configuration; empty if it has no session.
     */
    QString portInUse( const QString& portName ) const;

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
    /**
     * startSession(), saying why it was refused: as the user was notified;
     * empty if it started.
     */
    QString launch( const SerialConfig& config, const QString& savePath, const QString& owner );

    /** Tell the user why a capture is not started (Host::warn()), and return it. */
    QString refuse( const QString& message );

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

    /** The port of the session the configuration @p name started; empty if none. */
    QString portOf( const QString& name ) const;

    Host& host_;
    QStringList ports_; ///< Result of the last port scan.
    QMap<QString, SerialProcess*> sessions_;
    QMap<QString, QString> owners_; ///< Port → the configuration that started its session.
};

} // namespace serial_monitor
