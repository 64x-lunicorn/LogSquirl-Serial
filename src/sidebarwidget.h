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
 * @file sidebarwidget.h
 * @brief The plugin's tab "Serial" in LogSquirl's sidebar.
 *
 *   [New configuration…]           [Stop all]
 *   ┌ tile: board ──────────────────────────┐
 *   └───────────────────────────────────────┘
 *   ┌ tile: console ────────────────────────┐
 *   └───────────────────────────────────────┘
 *
 * One tile (connectiontile.h) per saved configuration (profiles.h), in the
 * order they were saved, with what the configuration is (its port, its
 * frame such as `115200 8N1`, its flow control) and a Start / Stop button.
 * Tiles on different ports run at the same time, one session each
 * (sessions.h).
 *
 * A tile's session is the one Sessions knows it started (Sessions::portOf()
 * of its name), so a tile shows its capture as running also after its port
 * was changed in the dialog.  A tile whose port is captured by another
 * configuration says so in its details ("<port> is in use by …"); its Start
 * is refused by the sessions with a notification that says the same.
 *
 * While its capture runs, a tile shows what it does, in its details:
 *
 *   running · 12 lines/s · 0:05:31
 *   1,234 lines · 56.7 KiB received
 *   file 61.2 KiB · new file at 1.0 GiB · 0 rotations
 *
 * the lines per second since the refresh before and the running time
 * (h:mm:ss); the lines written, [TX] ones too, and the bytes received; the
 * current file's size, the size at which the next file starts ("no size
 * limit" without one) and the rotations so far.  Below them are its controls: a command field, the
 * line ending (CRLF, LF, CR or none; CRLF until another is chosen, kept per tile until LogSquirl
 * quits), ➤ Send (or Return) and ↻ Rotate.  The command typed and not sent
 * yet is kept per tile too, so a reload() after another configuration was
 * saved or deleted does not lose it.  Send writes the command to the
 * port the tile's session captures (Sessions::sendToSession()) and empties
 * the field; Rotate goes on in a new file and tab
 * (Sessions::rotateSession()).  A stopped tile shows its main facts only:
 * no details unless its port is in use, no controls.
 *
 * Edit… opens the dialog with the configuration and New configuration…
 * opens it empty; the plugin carries those out (editRequested() and
 * newConfigurationRequested()).  Delete asks, then deletes the saved
 * configuration through the profile store, not the dialog; when the store
 * cannot, a message box says why.  The tiles follow the store: reload()
 * shows the configurations saved now whenever it changed.  They follow the
 * sessions' signals: a start, stop or rotation shows at once.  The counters
 * change on every line, without a signal: while some capture runs, a timer
 * reads them once a second (refresh()); while none runs, nothing is polled.
 *
 * Children named for tests: "newConfiguration", "stopAll", "empty" (the
 * hint shown without configurations), and the tiles ("tile"), each with
 * its "controls": "command", "lineEnding", "send", "rotate".
 *
 * OWNERSHIP
 * ─────────
 * Created by logsquirl_plugin_init() and registered with
 * register_sidebar_tab.  logsquirl_plugin_shutdown() unregisters and
 * deletes it, before the store and the sessions it shows.
 */

#pragma once

#include "serialprocess.h"

#include <QElapsedTimer>
#include <QHash>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace serial_monitor {

class ConnectionTile;
class ProfileStore;
class Sessions;

class SidebarWidget : public QWidget {
    Q_OBJECT

public:
    /// Shows the configurations of @p profiles and the sessions of
    /// @p sessions, which must both outlive it.
    SidebarWidget( ProfileStore* profiles, Sessions* sessions, QWidget* parent = nullptr );

    /// Show the configurations saved now.
    void reload();

    /// The tile of the configuration @p name; nullptr if there is none.
    ConnectionTile* tile( const QString& name ) const;

    /// Read the running captures' counters now and show them; the timer
    /// calls it once a second while some capture runs.
    void refresh();

Q_SIGNALS:
    /// New configuration… was clicked.
    void newConfigurationRequested();

    /// Edit… was clicked on the configuration @p name.
    void editRequested( const QString& name );

private:
    /// Start a capture of the configuration @p name, as its session's owner.
    void start( const QString& name );

    /// Stop the capture the configuration @p name started.
    void stop( const QString& name );

    /// Ask whether to delete the configuration @p name, and if so delete
    /// it, or say why it could not be.
    void confirmDelete( const QString& name );

    /// Rotate the log of the capture the configuration @p name started.
    void rotate( const QString& name );

    /// Send the text of @p command with @p lineEnding to the port the
    /// configuration @p name captures, and empty @p command once it is sent.
    void sendFrom( const QString& name, QLineEdit* command, TxLineEnding lineEnding );

    /// The session the configuration @p name started; nullptr if none runs.
    const SerialProcess* sessionOf( const QString& name ) const;

    /// The tile controls of the configuration @p name's running capture:
    /// "command", "lineEnding", "send" and "rotate".
    QWidget* controlsFor( const QString& name );

    /// Show what the configuration of @p tile captures, or why it cannot.
    void updateTile( ConnectionTile* tile );

    /// Update every tile and Stop all, after a session started, stopped or
    /// rotated, and run the timer while some session runs.
    void updateTiles();

    ProfileStore* profiles_;
    Sessions* sessions_;
    QPushButton* stopAll_;
    QLabel* empty_;
    QVBoxLayout* tiles_;
    QTimer* timer_;
    QHash<QString, ConnectionTile*> tileByName_;
    QHash<QString, QString> savedPortByName_; ///< The port each configuration has saved.
    /// The line ending chosen on each tile, kept when the tiles are rebuilt.
    QHash<QString, TxLineEnding> lineEndingByName_;
    /// The command typed on each tile and not sent yet, kept when the tiles
    /// are rebuilt.
    QHash<QString, QString> commandByName_;

    /// What a running capture had written at the last refresh().
    struct Sample {
        qint64 lines = 0;
        qint64 rate = 0; ///< Lines per second since the refresh before.
        QElapsedTimer since;
    };
    QHash<QString, Sample> samples_; ///< By configuration name.
};

} // namespace serial_monitor
