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
 * @brief Sidebar panel for serial port session management.
 *
 * This widget is registered as a tab in the LogSquirl sidebar via the
 * host API's register_sidebar_tab().  It provides:
 *
 *   - Port combo box with refresh button
 *   - Serial settings (baud, data bits, stop bits, parity, flow, timestamps)
 *   - Start / Stop capture buttons
 *   - Active sessions list showing port name, line count, and
 *     per-session Rotate / Stop buttons
 *   - A "Stop All" button
 *
 * The panel is a view of the Sessions (sessions.h), as is the dialog
 * still available via the Plugins menu: it calls them, and updates on
 * their signals.
 */

#pragma once

#include "serialprocess.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

namespace serial_monitor {

class Sessions;

/**
 * Sidebar panel widget for serial port session control.
 *
 * Embeds port selection, serial settings, and an active-sessions list.
 * All actual session logic is in the Sessions.
 */
class SidebarWidget : public QWidget {
    Q_OBJECT

public:
    /**
     * Construct the sidebar panel.
     *
     * @param sessions  The captures the panel shows and controls.
     *                  Must outlive this widget.
     * @param parent    QWidget parent.
     */
    explicit SidebarWidget( Sessions* sessions, QWidget* parent = nullptr );

    /** Set the serial settings to the saved defaults (baud rate, timestamps). */
    void loadDefaults();

private Q_SLOTS:
    /** Refill the port combo box and the session list from the Sessions. */
    void updatePortList();

    /** Start capture for the currently selected port. */
    void startCapture();

    /** Stop capture for the currently selected port. */
    void stopSelectedCapture();

    /** Stop all active sessions. */
    void stopAllCaptures();

    /** Send the contents of the send input to the active session. */
    void sendCommand();

private:
    /** Build or rebuild the sessions list widget contents. */
    void rebuildSessionList();

    /** Return the port name of the current combo selection. */
    QString currentPortName() const;

    /** Build a SerialConfig from the current sidebar UI selections. */
    SerialConfig buildConfig() const;

    /** Update button enabled/disabled states. */
    void updateUiState();

    /** Generate a save path for a new session based on log directory and port name. */
    QString generateSavePath( const QString& portName ) const;

    /** Load the persisted log directory from plugin config. */
    void loadLogDir();

    /** Persist the log directory to plugin config. */
    void saveLogDir();

    // ── Shared state ─────────────────────────────────────────────────
    Sessions* sessions_ = nullptr; ///< The captures shown; not owned.

    // ── UI elements ──────────────────────────────────────────────────
    QComboBox* portCombo_ = nullptr;
    QPushButton* refreshButton_ = nullptr;

    // Serial settings
    QComboBox* baudCombo_ = nullptr;
    QComboBox* dataBitsCombo_ = nullptr;
    QComboBox* stopBitsCombo_ = nullptr;
    QComboBox* parityCombo_ = nullptr;
    QComboBox* flowControlCombo_ = nullptr;
    QCheckBox* timestampCheckBox_ = nullptr;

    // Action buttons
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* stopAllButton_ = nullptr;

    // Send command
    QLineEdit* sendEdit_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QComboBox* lineEndingCombo_ = nullptr;

    // Log directory
    QLineEdit* logDirEdit_ = nullptr;
    bool logDirSaveFailed_ = false; ///< The last save of the log directory failed.
    QPushButton* logDirBrowseButton_ = nullptr;

    // Sessions
    QListWidget* sessionList_ = nullptr;
    QLabel* statusLabel_ = nullptr;
};

} // namespace serial_monitor
