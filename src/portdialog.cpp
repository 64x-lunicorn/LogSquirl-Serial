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
 * @file portdialog.cpp
 * @brief Implementation of the dialog that edits a saved port configuration.
 */

#include "portdialog.h"

#include "baudrate.h"
#include "plugin.h"
#include "profiles.h"
#include "sessions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace serial_monitor {

namespace {

/// Select the entry of @p combo whose data is @p value, if it has one.
void selectData( QComboBox* combo, int value )
{
    if ( const int index = combo->findData( value ); index >= 0 ) {
        combo->setCurrentIndex( index );
    }
}

} // namespace

PortDialog::PortDialog( ProfileStore* profiles, Sessions* sessions, QWidget* parent )
    : QDialog( parent )
    , profiles_( profiles )
    , sessions_( sessions )
    , profileCombo_( new QComboBox( this ) )
    , saveProfileButton_( new QPushButton( QStringLiteral( "Save" ), this ) )
    , deleteProfileButton_( new QPushButton( QStringLiteral( "Delete" ), this ) )
    , portCombo_( new QComboBox( this ) )
    , refreshButton_( new QPushButton( QStringLiteral( "⟳ Refresh" ), this ) )
    , baudCombo_( new QComboBox( this ) )
    , dataBitsCombo_( new QComboBox( this ) )
    , stopBitsCombo_( new QComboBox( this ) )
    , parityCombo_( new QComboBox( this ) )
    , flowControlCombo_( new QComboBox( this ) )
    , timestampCheckBox_(
          new QCheckBox( QStringLiteral( "Prepend a timestamp to each line" ), this ) )
    , logDirEdit_( new QLineEdit( this ) )
    , browseLogDirButton_( new QPushButton( QStringLiteral( "…" ), this ) )
    , statusLabel_( new QLabel( this ) )
{
    setWindowTitle( QStringLiteral( "Serial port configuration" ) );
    setMinimumWidth( 480 );

    // A top-level window of the plugin's own: when it is open while the
    // user closes LogSquirl's main window, LogSquirl must still quit.
    setAttribute( Qt::WA_QuitOnClose, false );

    profileCombo_->setObjectName( "profile" );
    profileCombo_->setEditable( true );
    profileCombo_->setInsertPolicy( QComboBox::NoInsert );
    profileCombo_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
    profileCombo_->lineEdit()->setPlaceholderText( QStringLiteral( "Name of the configuration" ) );
    profileCombo_->setToolTip(
        QStringLiteral( "Enter a name and Save the settings below under it: the configuration "
                        "gets a tile in the Serial sidebar, where it is started. Choose a saved "
                        "configuration to change it" ) );
    saveProfileButton_->setObjectName( "saveProfile" );
    saveProfileButton_->setDefault( true );
    deleteProfileButton_->setObjectName( "deleteProfile" );
    deleteProfileButton_->setAutoDefault( false );

    portCombo_->setObjectName( "port" );
    portCombo_->setEditable( true );
    portCombo_->setInsertPolicy( QComboBox::NoInsert );
    portCombo_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
    portCombo_->lineEdit()->setPlaceholderText( QStringLiteral( "e.g. /dev/ttyUSB0 or COM3" ) );
    portCombo_->setToolTip(
        QStringLiteral( "The serial port: one found by the last scan, or type in another" ) );
    refreshButton_->setObjectName( "refresh" );
    refreshButton_->setAutoDefault( false );
    refreshButton_->setToolTip( QStringLiteral( "Scan for serial ports again" ) );

    baudCombo_->setObjectName( "baudRate" );
    initBaudRateCombo( baudCombo_ );

    dataBitsCombo_->setObjectName( "dataBits" );
    dataBitsCombo_->addItem( "5", QSerialPort::Data5 );
    dataBitsCombo_->addItem( "6", QSerialPort::Data6 );
    dataBitsCombo_->addItem( "7", QSerialPort::Data7 );
    dataBitsCombo_->addItem( "8", QSerialPort::Data8 );

    stopBitsCombo_->setObjectName( "stopBits" );
    stopBitsCombo_->addItem( "1", QSerialPort::OneStop );
    stopBitsCombo_->addItem( "1.5", QSerialPort::OneAndHalfStop );
    stopBitsCombo_->addItem( "2", QSerialPort::TwoStop );

    parityCombo_->setObjectName( "parity" );
    parityCombo_->addItem( "None", QSerialPort::NoParity );
    parityCombo_->addItem( "Even", QSerialPort::EvenParity );
    parityCombo_->addItem( "Odd", QSerialPort::OddParity );
    parityCombo_->addItem( "Mark", QSerialPort::MarkParity );
    parityCombo_->addItem( "Space", QSerialPort::SpaceParity );

    flowControlCombo_->setObjectName( "flowControl" );
    flowControlCombo_->addItem( "None", QSerialPort::NoFlowControl );
    flowControlCombo_->addItem( "Hardware (RTS/CTS)", QSerialPort::HardwareControl );
    flowControlCombo_->addItem( "Software (XON/XOFF)", QSerialPort::SoftwareControl );

    timestampCheckBox_->setObjectName( "timestamps" );
    timestampCheckBox_->setToolTip(
        QStringLiteral( "Add a [YYYY-MM-DD HH:mm:ss.zzz] prefix to each received line" ) );

    logDirEdit_->setObjectName( "logDir" );
    const auto defaultDir = defaultLogDir();
    const auto whenEmpty = defaultDir.isEmpty()
                               ? QStringLiteral( "temporary files, removed when LogSquirl quits" )
                               : QDir::toNativeSeparators( defaultDir );
    logDirEdit_->setPlaceholderText( "(empty: " + whenEmpty + ")" );
    logDirEdit_->setToolTip( QStringLiteral( "Directory where log files are saved and kept.\n"
                                             "When empty: %1.\n"
                                             "Files are named: YYYY-MM-dd_HHmmss_<port>.log" )
                                 .arg( whenEmpty ) );
    browseLogDirButton_->setObjectName( "browseLogDir" );
    browseLogDirButton_->setAutoDefault( false );
    browseLogDirButton_->setFixedWidth( 30 );
    browseLogDirButton_->setToolTip( QStringLiteral( "Browse for the log directory" ) );

    statusLabel_->setObjectName( "status" );
    statusLabel_->setWordWrap( true );
    statusLabel_->setTextInteractionFlags( Qt::TextSelectableByMouse );

    // ── Layout ───────────────────────────────────────────────────────
    auto* profileRow = new QHBoxLayout;
    profileRow->addWidget( profileCombo_, 1 );
    profileRow->addWidget( saveProfileButton_ );
    profileRow->addWidget( deleteProfileButton_ );

    auto* portRow = new QHBoxLayout;
    portRow->addWidget( portCombo_, 1 );
    portRow->addWidget( refreshButton_ );

    auto* top = new QFormLayout;
    top->addRow( QStringLiteral( "Name:" ), profileRow );
    top->addRow( QStringLiteral( "Port:" ), portRow );

    auto* settingsGroup = new QGroupBox( QStringLiteral( "Settings" ), this );
    auto* settingsLayout = new QFormLayout( settingsGroup );
    settingsLayout->addRow( QStringLiteral( "Baud rate:" ), baudCombo_ );
    settingsLayout->addRow( QStringLiteral( "Data bits:" ), dataBitsCombo_ );
    settingsLayout->addRow( QStringLiteral( "Stop bits:" ), stopBitsCombo_ );
    settingsLayout->addRow( QStringLiteral( "Parity:" ), parityCombo_ );
    settingsLayout->addRow( QStringLiteral( "Flow control:" ), flowControlCombo_ );
    settingsLayout->addRow( timestampCheckBox_ );

    auto* logDirGroup = new QGroupBox( QStringLiteral( "Log directory" ), this );
    auto* logDirLayout = new QHBoxLayout( logDirGroup );
    logDirLayout->addWidget( logDirEdit_ );
    logDirLayout->addWidget( browseLogDirButton_ );

    auto* layout = new QVBoxLayout( this );
    layout->addLayout( top );
    layout->addWidget( settingsGroup );
    layout->addWidget( logDirGroup );
    layout->addWidget( statusLabel_ );
    layout->addStretch();

    // ── Connections ──────────────────────────────────────────────────
    // Every slot runs in the host's event loop: nothing may throw into it.
    connect( profileCombo_, &QComboBox::currentIndexChanged, this, [ this ]( int index ) {
        guarded( "loading a serial configuration", [ this, index ] { loadProfile( index ); } );
    } );
    connect( saveProfileButton_, &QPushButton::clicked, this, [ this ] {
        guarded( "saving a serial configuration", [ this ] { saveProfile(); } );
    } );
    connect( deleteProfileButton_, &QPushButton::clicked, this, [ this ] {
        guarded( "deleting a serial configuration",
                 [ this ] { deleteConfiguration( profileCombo_->currentText().trimmed() ); } );
    } );
    connect( refreshButton_, &QPushButton::clicked, this, [ this ] {
        guarded( "scanning for serial ports", [ this ] { sessions_->refreshPorts(); } );
    } );
    connect( browseLogDirButton_, &QPushButton::clicked, this,
             [ this ] { guarded( "choosing the log directory", [ this ] { browseLogDir(); } ); } );
    connect( sessions_, &Sessions::portsChanged, this,
             [ this ] { guarded( "listing the serial ports", [ this ] { fillPortList(); } ); } );
    // Saved here, or deleted from a tile: the list follows, the name entered stays.
    connect( profiles_, &ProfileStore::changed, this, [ this ] {
        guarded( "listing the serial configurations",
                 [ this ] { fillProfileList( profileCombo_->currentText().trimmed() ); } );
    } );

    // The Sessions have scanned the ports already.
    fillPortList();
    newConfiguration();
}

PortProfile PortDialog::currentProfile() const
{
    PortProfile profile;
    profile.name = profileCombo_->currentText().trimmed();
    auto& serial = profile.serial;
    serial.portName = portCombo_->currentText().trimmed();
    serial.baudRate = baudRateFrom( baudCombo_ );
    serial.dataBits = static_cast<QSerialPort::DataBits>( dataBitsCombo_->currentData().toInt() );
    serial.stopBits = static_cast<QSerialPort::StopBits>( stopBitsCombo_->currentData().toInt() );
    serial.parity = static_cast<QSerialPort::Parity>( parityCombo_->currentData().toInt() );
    serial.flowControl
        = static_cast<QSerialPort::FlowControl>( flowControlCombo_->currentData().toInt() );
    serial.timestamps = timestampCheckBox_->isChecked();
    profile.logDir = logDirEdit_->text().trimmed();
    return profile;
}

void PortDialog::showProfile( const PortProfile& profile )
{
    const auto& serial = profile.serial;
    portCombo_->setEditText( serial.portName );
    if ( serial.baudRate > 0 ) {
        selectBaudRate( baudCombo_, serial.baudRate );
    }
    selectData( dataBitsCombo_, serial.dataBits );
    selectData( stopBitsCombo_, serial.stopBits );
    selectData( parityCombo_, serial.parity );
    selectData( flowControlCombo_, serial.flowControl );
    timestampCheckBox_->setChecked( serial.timestamps );
    logDirEdit_->setText( profile.logDir );
}

void PortDialog::newConfiguration()
{
    fillProfileList( {} );
    showProfile( profiles_->newProfile() );
    statusLabel_->setText(
        QStringLiteral( "Enter a name and the settings, then Save: the configuration gets a tile "
                        "in the Serial sidebar, where it is started." ) );
    profileCombo_->setFocus();
}

void PortDialog::editConfiguration( const QString& name )
{
    fillProfileList( name );
    const int index = profileCombo_->findText( name );
    if ( index >= 0 ) {
        loadProfile( index );
    }
}

void PortDialog::fillProfileList( const QString& current )
{
    fillingProfiles_ = true;
    profileCombo_->clear();
    for ( const auto& profile : profiles_->list() ) {
        profileCombo_->addItem( profile.name );
    }
    profileCombo_->setCurrentIndex( profileCombo_->findText( current ) );
    profileCombo_->setEditText( current );
    fillingProfiles_ = false;
}

void PortDialog::fillPortList()
{
    const auto current = portCombo_->currentText();
    portCombo_->clear();
    portCombo_->addItems( sessions_->ports() );
    portCombo_->setEditText( current );
}

void PortDialog::loadProfile( int index )
{
    if ( fillingProfiles_ || index < 0 ) {
        return;
    }
    const auto name = profileCombo_->itemText( index );
    const auto profile = profiles_->find( name );
    if ( !profile ) {
        return;
    }
    showProfile( *profile );
    statusLabel_->setText( QStringLiteral( "Configuration %1 loaded." ).arg( name ) );
}

void PortDialog::saveProfile()
{
    const auto profile = currentProfile();
    if ( profile.name.isEmpty() ) {
        statusLabel_->setText( QStringLiteral( "Enter a name for the configuration to save." ) );
        return;
    }
    if ( const auto problem = startProblem( profile ); !problem.isEmpty() ) {
        statusLabel_->setText( problem );
        return;
    }
    // The list follows the store (ProfileStore::changed), the name chosen.
    if ( const auto problem = profiles_->upsert( profile ); !problem.isEmpty() ) {
        statusLabel_->setText( problem );
        return;
    }
    if ( !sessions_->portOf( profile.name ).isEmpty() ) {
        statusLabel_->setText( QStringLiteral( "Configuration %1 saved. Its capture runs on with "
                                               "the settings it started with: stop and start it "
                                               "to use the new ones." )
                                   .arg( profile.name ) );
        return;
    }
    statusLabel_->setText(
        QStringLiteral( "Configuration %1 saved: start it from its tile in the Serial sidebar." )
            .arg( profile.name ) );
}

void PortDialog::deleteConfiguration( const QString& name )
{
    if ( const auto problem = profiles_->remove( name ); !problem.isEmpty() ) {
        statusLabel_->setText( problem );
        return;
    }
    fillProfileList( {} );
    statusLabel_->setText( QStringLiteral( "Configuration %1 deleted." ).arg( name ) );
}

void PortDialog::browseLogDir()
{
    const auto dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral( "Select Log Directory" ), logDirEdit_->text() );
    if ( !dir.isEmpty() ) {
        logDirEdit_->setText( dir );
    }
}

} // namespace serial_monitor
