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
 * @file portdialog.h
 * @brief Non-modal dialog for the settings of a saved port configuration.
 *
 * Opened from the sidebar (New configuration…, a tile's Edit…) and from
 * Plugins ▸ Serial Monitor…  It provides:
 *
 *   - the configuration's name: choosing a saved one fills in its settings,
 *     Save saves the settings under the name entered (a new tile in the
 *     sidebar, or the tile of that name updated), Delete removes it.  While
 *     a saved configuration is edited (chosen, opened with Edit…, or saved
 *     last), a new name renames it and its tile; that is refused while it
 *     captures, or when another configuration has the name
 *   - the port: the ports found by the last scan, Refresh to scan again;
 *     any other port can be typed in (a device not plugged in yet)
 *   - the baud rate (a common one, or any other typed in), data bits, stop
 *     bits, parity and flow control
 *   - whether each line gets a [YYYY-MM-DD HH:mm:ss.zzz] timestamp
 *   - the log directory; empty, `logs` in the plugin's configuration
 *     directory (profiles.h, ProfileStore::defaultLogDir())
 *   - a status line: what was saved, or what is missing
 *
 * The captures are started and stopped from their tiles in the sidebar
 * (sidebarwidget.h), not here.
 *
 * SETTINGS
 * ────────
 * The configurations are saved by name through the profile store
 * (profiles.h), the one owner of serial.ini; the dialog reads
 * and writes no file itself.  A new configuration proposes the defaults of
 * Configure and the log directory saved last.  The list of names follows
 * the store, also when a configuration is deleted from its tile.
 *
 * Children named for tests: "profile", "saveProfile", "deleteProfile",
 * "port", "refresh", "baudRate", "dataBits", "stopBits", "parity",
 * "flowControl", "timestamps", "logDir", "browseLogDir", "status".
 *
 * OWNERSHIP
 * ─────────
 * A member of the plugin instance (plugin.cpp), destroyed before the store
 * and the sessions it uses.
 */

#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace serial_monitor {

class ProfileStore;
class Sessions;
struct PortProfile;

class PortDialog : public QDialog {
    Q_OBJECT

public:
    /// A dialog on the configurations of @p profiles, offering the ports
    /// of @p sessions, which also tell when a configuration it saves
    /// captures; both must outlive it.
    PortDialog( ProfileStore* profiles, Sessions* sessions, QWidget* parent = nullptr );

    /// Set up an empty new configuration, with the default settings.
    void newConfiguration();

    /// Fill in the settings of the saved configuration @p name.
    void editConfiguration( const QString& name );

private:
    /// Delete the saved configuration @p name; the status line says what
    /// happened, or why it was not deleted.
    void deleteConfiguration( const QString& name );

    /// The settings entered, as a configuration named as entered.
    PortProfile currentProfile() const;

    /// Fill in the settings of @p profile.
    void showProfile( const PortProfile& profile );

    /// Fill in the settings of the saved configuration chosen in the list.
    void loadProfile( int index );

    /// Save the settings entered as the configuration named in the list;
    /// a new name renames the configuration edited.
    void saveProfile();

    /// Fill the list with the saved configurations' names, @p current chosen.
    void fillProfileList( const QString& current );

    /// Offer the ports of the last scan, keeping the port entered.
    void fillPortList();

    /// Choose the log directory.
    void browseLogDir();

    ProfileStore* profiles_;
    Sessions* sessions_;
    QComboBox* profileCombo_;
    QPushButton* saveProfileButton_;
    QPushButton* deleteProfileButton_;
    QComboBox* portCombo_;
    QPushButton* refreshButton_;
    QComboBox* baudCombo_;
    QComboBox* dataBitsCombo_;
    QComboBox* stopBitsCombo_;
    QComboBox* parityCombo_;
    QComboBox* flowControlCombo_;
    QCheckBox* timestampCheckBox_;
    QLineEdit* logDirEdit_;
    QPushButton* browseLogDirButton_;
    QLabel* statusLabel_;
    bool fillingProfiles_ = false; ///< The list is being filled: nothing is loaded.
    /// The saved configuration the dialog edits (loaded or saved last): Save
    /// renames it when the name entered differs.  Empty for a new one.
    QString editing_;
};

} // namespace serial_monitor
