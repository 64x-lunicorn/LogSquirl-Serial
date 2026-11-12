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
 * @file profiles.h
 * @brief Saved port configurations: a port's settings saved under a name.
 *
 * A configuration (profile) is a port with its line settings (baud rate,
 * data bits, stop bits, parity, flow control), whether lines get a
 * timestamp, and the directory its captures are written to.  Each one is a
 * tile in the sidebar (sidebarwidget.h), started from there; the dialog
 * (portdialog.h) edits them.
 *
 * PROFILE STORE
 * ─────────────
 * Configurations are saved in serial.ini in the plugin's configuration
 * directory (get_config_dir), as the array "profiles", in the order they
 * were first saved, next to the defaults Configure saves (group "serial").
 * A ProfileStore opened on that file is their one owner: the dialog and the
 * sidebar list, find, save and delete them through it, and it remembers the
 * log directory saved last, which a new configuration proposes (the key the
 * sidebar of earlier versions saved its log directory under).  When it
 * cannot save or delete, it says why, in words for the user.  It reads the
 * file on each call, so two stores on one file agree.
 *
 * Tested on the configuration directory of a FakeHost (profiles_test.cpp),
 * and through the dialog and the sidebar.
 */

#pragma once

#include "serialprocess.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace serial_monitor {

struct PortProfile {
    QString name;
    /// The port and its settings.  Its txLineEnding and maxFileBytes are
    /// not saved: the size limit is Configure's, for every capture.
    SerialConfig serial;
    /// Where its captures are written; empty: defaultLogDir().
    QString logDir;
};

/// Where a configuration without a log directory writes its captures:
/// `logs` in the plugin's configuration directory, where they are kept.
/// Empty without a configuration directory: each capture then writes to a
/// temporary file, removed when LogSquirl quits.
QString defaultLogDir();

/// The frame of @p config as a terminal shows it: "115200 8N1".
QString frameOf( const SerialConfig& config );

/// @p flowControl in words: "no flow control", "RTS/CTS" or "XON/XOFF".
QString flowControlName( QSerialPort::FlowControl flowControl );

/// What keeps @p profile from starting a capture, in words for the user;
/// empty if nothing (whether its port opens shows at Start).
QString startProblem( const PortProfile& profile );

/// The session configuration of @p profile, with the size limit set in
/// Configure.
SerialConfig sessionConfig( const PortProfile& profile );

/// A new log file for a capture of @p profile, named
/// YYYY-MM-dd_HHmmss_<port>.log, in its log directory, which is created if
/// it does not exist; empty when it has none and there is no default (a
/// temporary file then).  A log directory that is not a full path is
/// returned as it is, for SerialProcess::start() to refuse with a message.
QString savePathFor( const PortProfile& profile );

/// The saved configurations of one settings file; the only code that reads
/// or writes them.
class ProfileStore : public QObject {
    Q_OBJECT

public:
    /// Whether the configuration @p name captures: it is not deleted then.
    using IsRunning = std::function<bool( const QString& name )>;

    /// The store of @p settingsFile, which need not exist yet; empty, the
    /// store has no configuration and saves none (no host to say where).
    /// @p isRunning tells which configurations capture; none if it is empty.
    explicit ProfileStore( const QString& settingsFile, IsRunning isRunning = {},
                           QObject* parent = nullptr );

    /// The file the store reads and writes.
    QString settingsFile() const;

    /// The saved configurations, in the order they were first saved.
    QList<PortProfile> list() const;

    /// The configuration named @p name; empty if there is none.
    std::optional<PortProfile> find( const QString& name ) const;

    /// The settings a new configuration starts with: the defaults of
    /// Configure (baud rate, timestamps), and the log directory saved last.
    PortProfile newProfile() const;

    /// Save @p profile under its name, replacing the configuration of that
    /// name where it is in the list, or after the others, and remember its
    /// log directory for newProfile().  Empty if it was saved, else why not.
    QString upsert( const PortProfile& profile );

    /// Delete the configuration named @p name, unless it captures.  Empty
    /// if it was deleted, else why not.
    QString remove( const QString& name );

Q_SIGNALS:
    /// A configuration was saved or deleted.
    void changed();

private:
    /// Write @p profiles, replacing those saved, and @p logDir as the log
    /// directory to remember if set; whether the file was written.
    bool write( const QList<PortProfile>& profiles, const std::optional<QString>& logDir );

    QString settingsFile_;
    IsRunning isRunning_;
};

} // namespace serial_monitor
