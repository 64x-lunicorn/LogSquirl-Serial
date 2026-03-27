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
 * @file portwidget.h
 * @brief Non-modal QDialog for serial port selection and session control.
 *
 * The PortWidget is opened from the Plugins → Serial Monitor… menu entry.
 * It provides:
 *
 *   - A combo box listing discovered serial ports
 *   - A "Refresh" button to re-scan for ports
 *   - Serial parameter controls (baud, data bits, stop bits, parity, flow)
 *   - A "Timestamps" checkbox for optional line timestamping
 *   - A "Start" button to begin capture for the selected port
 *   - A "Stop" button to end the active session for the selected port
 *   - A "Stop All" button (shown when multiple sessions are active)
 *   - A "Save to file" checkbox with a file path selector
 *   - A status label showing active session count
 *
 * Multiple ports can be captured simultaneously — each gets its own
 * SerialProcess, temp file, and LogSquirl tab.
 *
 * OWNERSHIP
 * ─────────
 * The dialog is created by the menu action callback and deleted by
 * logsquirl_plugin_shutdown().  Active SerialProcess instances are
 * children of the PortWidget and are cleaned up automatically.
 */

#pragma once

#include "serialprocess.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>

namespace serial_monitor {

/**
 * Non-modal dialog for managing serial port capture sessions.
 *
 * When the user clicks "Start", a SerialProcess is created for the
 * selected port and the resulting temp file is opened in LogSquirl
 * via the host API.
 */
class PortWidget : public QDialog {
    Q_OBJECT

  public:
    explicit PortWidget( QWidget* parent = nullptr );
    ~PortWidget() override = default;

    /** Stop all active sessions.  Called during plugin shutdown. */
    void stopAll();

    /** Number of currently running sessions. */
    int activeSessionCount() const;

    /** Return the port names of all currently active sessions. */
    QStringList activePorts() const;

    /**
     * Rotate the log for an active session: the old temp file is preserved
     * (tab stays open), a new temp file is created, and a new tab opens.
     *
     * @param portName  Port name of the session to rotate.
     */
    void rotateSession( const QString& portName );

    /**
     * Start capture for a specific port.
     *
     * @param config    Serial port configuration.
     * @param savePath  Optional path to a .log file for persistent saving.
     * @return true if the session started successfully, false otherwise.
     */
    bool startSession( const SerialConfig& config, const QString& savePath = {} );

    /**
     * Stop capture for a specific port.
     *
     * @param portName  Port name of the session to stop.
     */
    void stopSession( const QString& portName );

    /**
     * Return the current line count for an active session.
     *
     * @param portName  Port name to query.
     * @return Line count, or 0 if the session is not active.
     */
    qint64 sessionLineCount( const QString& portName ) const;

    /**
     * Check whether a session is currently running for the given port.
     */
    bool isSessionActive( const QString& portName ) const;

  private Q_SLOTS:
    /** Re-scan for serial ports and update the combo box. */
    void refreshPorts();

    /** Start capture for the currently selected port. */
    void startCapture();

    /** Stop capture for the currently selected port. */
    void stopCapture();

    /** Stop all active sessions. */
    void stopAllCaptures();

    /** Let the user browse for a save file path. */
    void browseSavePath();

    /** Handle a session ending (cleanup bookkeeping). */
    void onSessionFinished( const QString& portName );

    /** Handle a session error. */
    void onSessionError( const QString& portName, const QString& message );

  private:
    /** Update UI state (button enable/disable, status label). */
    void updateUiState();

    /** Return the port name of the currently selected entry, or empty. */
    QString currentPortName() const;

    /** Build a SerialConfig from the current UI selections. */
    SerialConfig buildConfig() const;

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

    // Save-to-file
    QCheckBox* saveCheckBox_ = nullptr;
    QLineEdit* savePathEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;

    // Status
    QLabel* statusLabel_ = nullptr;

    // ── Active sessions (portName → SerialProcess*) ─────────────────
    QMap<QString, SerialProcess*> sessions_;
};

} // namespace serial_monitor
