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
 * @file connectiontile.h
 * @brief One saved configuration as a tile (card) in the sidebar.
 *
 *   ╭───────────────────────────────────────╮
 *   │ ● board                    [■ Stop]   │  state dot, title, Start / Stop
 *   │ /dev/ttyUSB0                          │  summary: what the
 *   │ 115200 8N1 · no flow control          │  configuration is
 *   │ ───────────────────────────────────── │  separator, with the details
 *   │ running · 12 lines/s · 0:05:31        │  details (muted): what it does,
 *   │ 1,234 lines · 56.7 KiB received       │  or why it cannot start
 *   │ [command…            ]      [➤ Send]  │  controls of the running
 *   │ Line ending: [CRLF ▾]     [↻ Rotate]  │  capture, if any (setControls())
 *   │                               [Edit…] │  Delete only while it is idle
 *   ╰───────────────────────────────────────╯
 *
 * A card: a rounded 1 px frame on a background set off the sidebar's, the
 * frame in the accent colour while the capture is active.  Its colours are
 * taken from the palette, so a light and a dark theme both work, and follow
 * a change of the palette.  The dot is grey (stopped), amber (connecting),
 * green (running) or red (failed); the details say the same in words, for
 * whoever cannot tell the colours apart.
 *
 * A plain widget that knows nothing of serial ports: the sidebar
 * (sidebarwidget.h) sets its texts and state and acts on its signals.  It is
 * the tile of LogSquirl-NATS (src/connectiontile.h there), shared by the
 * LogSquirl plugins with saved connections; keep the two alike.  It adds
 * setControls(), a place for controls of the running capture, which the
 * NATS tile does not need (yet).
 *
 * Its children are named for tests: "state" (its property "state" is
 * stopped, connecting, running or failed), "title", "summary",
 * "separator", "details", "startStop", "edit", "delete".
 */

#pragma once

#include <QFrame>
#include <QPointer>
#include <QString>
#include <QStringList>

class QEvent;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace serial_monitor {

/// What a tile's capture does.
enum class TileState {
    Stopped,    ///< Idle: never started, stopped, or done.
    Connecting, ///< Started, not (yet, or again) connected; unused by serial ports.
    Running,    ///< Capturing.
    Failed,     ///< Ended by an error.
};

class ConnectionTile : public QFrame {
    Q_OBJECT

public:
    /// A tile titled @p name, stopped, without a summary or details.
    explicit ConnectionTile( const QString& name, QWidget* parent = nullptr );

    /// The name of the configuration it shows.
    QString name() const
    {
        return name_;
    }

    /// What the configuration is, one line each.
    void setSummary( const QStringList& lines );

    /// What it does or did, one line each; none hides them.
    void setDetails( const QStringList& lines );

    /// What it does.  While it connects or runs, the button stops it and
    /// it cannot be deleted (Delete is hidden).
    void setState( TileState state );

    /// Controls of the running capture (a serial port's Rotate and Send),
    /// shown below the details while it connects or runs.  The tile takes
    /// @p controls over, and deletes those it was given before.
    void setControls( QWidget* controls );

protected:
    void changeEvent( QEvent* event ) override;
    bool eventFilter( QObject* watched, QEvent* event ) override;

Q_SIGNALS:
    /// Start was clicked, the capture being stopped.
    void startClicked();
    /// Stop was clicked, the capture running.
    void stopClicked();
    /// Edit… was clicked.
    void editClicked();
    /// Delete was clicked (it cannot be while the capture runs).
    void deleteClicked();

private:
    /// Style the card from the palette around it, if that changed.
    void applyStyle();

    QString name_;
    bool active_ = false;
    /// The palette colours the stylesheet was made for.
    QString styledFor_;
    /// The parent whose palette changes it follows.
    QPointer<QWidget> watched_;
    QLabel* state_;
    QLabel* summary_;
    QFrame* separator_;
    QLabel* details_;
    QPushButton* startStop_;
    QPushButton* delete_;
    QVBoxLayout* layout_;
    /// What setControls() gave it, if any.
    QPointer<QWidget> controls_;
};

} // namespace serial_monitor
