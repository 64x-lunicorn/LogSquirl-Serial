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
 * @file portwidget.cpp
 * @brief Implementation of the PortWidget dialog.
 *
 * The dialog is a view of the Sessions: its buttons call them ("Start"
 * starts a capture of the selected port with the settings of the form,
 * "Stop" stops it), and it refreshes its port list, its buttons and its
 * status line on their signals, whichever view started or stopped a
 * session, or when one ended on its own.
 */

#include "portwidget.h"
#include "baudrate.h"
#include "plugin.h"
#include "sessions.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace serial_monitor {

// ── Construction ────────────────────────────────────────────────────────

PortWidget::PortWidget( Sessions* sessions, QWidget* parent )
    : QDialog( parent )
    , sessions_( sessions )
{
    setWindowTitle( "Serial Monitor" );
    setMinimumWidth( 480 );

    // A top-level window of the plugin's own: when it is open while the
    // user closes LogSquirl's main window, LogSquirl must still quit.
    setAttribute( Qt::WA_QuitOnClose, false );

    auto* mainLayout = new QVBoxLayout( this );

    // ── Port selection group ─────────────────────────────────────────
    auto* portGroup = new QGroupBox( "Port", this );
    auto* portLayout = new QVBoxLayout( portGroup );

    auto* portRow = new QHBoxLayout();
    portCombo_ = new QComboBox( this );
    portCombo_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
    portCombo_->setToolTip( "Select a serial port" );
    portRow->addWidget( portCombo_ );

    refreshButton_ = new QPushButton( "\u27F3 Refresh", this );
    refreshButton_->setObjectName( "refresh" );
    refreshButton_->setToolTip( "Refresh port list" );
    portRow->addWidget( refreshButton_ );
    portLayout->addLayout( portRow );

    mainLayout->addWidget( portGroup );

    // ── Serial settings group ────────────────────────────────────────
    auto* settingsGroup = new QGroupBox( "Settings", this );
    auto* settingsLayout = new QFormLayout( settingsGroup );

    baudCombo_ = new QComboBox( this );
    initBaudRateCombo( baudCombo_ );
    settingsLayout->addRow( "Baud rate:", baudCombo_ );

    dataBitsCombo_ = new QComboBox( this );
    dataBitsCombo_->addItem( "5", QSerialPort::Data5 );
    dataBitsCombo_->addItem( "6", QSerialPort::Data6 );
    dataBitsCombo_->addItem( "7", QSerialPort::Data7 );
    dataBitsCombo_->addItem( "8", QSerialPort::Data8 );
    dataBitsCombo_->setCurrentIndex( 3 ); // Data8
    settingsLayout->addRow( "Data bits:", dataBitsCombo_ );

    stopBitsCombo_ = new QComboBox( this );
    stopBitsCombo_->addItem( "1", QSerialPort::OneStop );
    stopBitsCombo_->addItem( "1.5", QSerialPort::OneAndHalfStop );
    stopBitsCombo_->addItem( "2", QSerialPort::TwoStop );
    stopBitsCombo_->setCurrentIndex( 0 ); // OneStop
    settingsLayout->addRow( "Stop bits:", stopBitsCombo_ );

    parityCombo_ = new QComboBox( this );
    parityCombo_->addItem( "None", QSerialPort::NoParity );
    parityCombo_->addItem( "Even", QSerialPort::EvenParity );
    parityCombo_->addItem( "Odd", QSerialPort::OddParity );
    parityCombo_->addItem( "Mark", QSerialPort::MarkParity );
    parityCombo_->addItem( "Space", QSerialPort::SpaceParity );
    parityCombo_->setCurrentIndex( 0 ); // NoParity
    settingsLayout->addRow( "Parity:", parityCombo_ );

    flowControlCombo_ = new QComboBox( this );
    flowControlCombo_->addItem( "None", QSerialPort::NoFlowControl );
    flowControlCombo_->addItem( "Hardware (RTS/CTS)", QSerialPort::HardwareControl );
    flowControlCombo_->addItem( "Software (XON/XOFF)", QSerialPort::SoftwareControl );
    flowControlCombo_->setCurrentIndex( 0 ); // NoFlowControl
    settingsLayout->addRow( "Flow control:", flowControlCombo_ );

    timestampCheckBox_ = new QCheckBox( "Prepend timestamp to each line", this );
    timestampCheckBox_->setObjectName( "timestamps" );
    timestampCheckBox_->setChecked( true );
    timestampCheckBox_->setToolTip( "Add [YYYY-MM-DD HH:mm:ss.zzz] prefix to each received line" );
    settingsLayout->addRow( timestampCheckBox_ );

    mainLayout->addWidget( settingsGroup );

    // ── Action buttons ───────────────────────────────────────────────
    auto* buttonRow = new QHBoxLayout();
    startButton_ = new QPushButton( "\u25B6 Start", this );
    startButton_->setToolTip( "Start serial capture for the selected port" );
    buttonRow->addWidget( startButton_ );

    stopButton_ = new QPushButton( "\u25A0 Stop", this );
    stopButton_->setToolTip( "Stop serial capture for the selected port" );
    buttonRow->addWidget( stopButton_ );

    stopAllButton_ = new QPushButton( "\u25A0 Stop All", this );
    stopAllButton_->setToolTip( "Stop all active serial sessions" );
    buttonRow->addWidget( stopAllButton_ );

    buttonRow->addStretch();
    mainLayout->addLayout( buttonRow );

    // ── Save-to-file group ───────────────────────────────────────────
    auto* saveGroup = new QGroupBox( "Save to file", this );
    auto* saveLayout = new QHBoxLayout( saveGroup );

    saveCheckBox_ = new QCheckBox( "Enable", this );
    saveCheckBox_->setToolTip( "Also save serial output to a file" );
    saveLayout->addWidget( saveCheckBox_ );

    savePathEdit_ = new QLineEdit( this );
    savePathEdit_->setPlaceholderText( "/path/to/output.log" );
    savePathEdit_->setEnabled( false );
    saveLayout->addWidget( savePathEdit_ );

    browseButton_ = new QPushButton( "\u2026", this );
    browseButton_->setFixedWidth( 30 );
    browseButton_->setEnabled( false );
    browseButton_->setToolTip( "Browse for a save file location" );
    saveLayout->addWidget( browseButton_ );

    mainLayout->addWidget( saveGroup );

    // ── Send command group ───────────────────────────────────────────
    auto* sendGroup = new QGroupBox( "Send Command", this );
    auto* sendLayout = new QVBoxLayout( sendGroup );

    auto* sendRow = new QHBoxLayout();
    sendEdit_ = new QLineEdit( this );
    sendEdit_->setPlaceholderText( "Enter command\u2026" );
    sendEdit_->setToolTip( "Type a command to send to the serial port" );
    sendRow->addWidget( sendEdit_ );

    sendButton_ = new QPushButton( "\u27A4 Send", this );
    sendButton_->setToolTip( "Send command to the active serial port" );
    sendRow->addWidget( sendButton_ );
    sendLayout->addLayout( sendRow );

    auto* lineEndingRow = new QHBoxLayout();
    auto* lineEndingLabel = new QLabel( "Line ending:", this );
    lineEndingRow->addWidget( lineEndingLabel );
    lineEndingCombo_ = new QComboBox( this );
    lineEndingCombo_->addItem( "CRLF (\\r\\n)", static_cast<int>( TxLineEnding::CRLF ) );
    lineEndingCombo_->addItem( "LF (\\n)", static_cast<int>( TxLineEnding::LF ) );
    lineEndingCombo_->addItem( "CR (\\r)", static_cast<int>( TxLineEnding::CR ) );
    lineEndingCombo_->addItem( "None", static_cast<int>( TxLineEnding::None ) );
    lineEndingCombo_->setCurrentIndex( 0 );
    lineEndingRow->addWidget( lineEndingCombo_ );
    sendLayout->addLayout( lineEndingRow );

    mainLayout->addWidget( sendGroup );

    // ── Status line ──────────────────────────────────────────────────
    statusLabel_ = new QLabel( this );
    statusLabel_->setStyleSheet( "color: gray; font-style: italic;" );
    mainLayout->addWidget( statusLabel_ );

    mainLayout->addStretch();

    // ── Connect signals ──────────────────────────────────────────────
    connect( refreshButton_, &QPushButton::clicked, sessions_, &Sessions::refreshPorts );
    connect( startButton_, &QPushButton::clicked, this, &PortWidget::startCapture );
    connect( stopButton_, &QPushButton::clicked, this, &PortWidget::stopCapture );
    connect( stopAllButton_, &QPushButton::clicked, this, &PortWidget::stopAllCaptures );
    connect( browseButton_, &QPushButton::clicked, this, &PortWidget::browseSavePath );
    connect( saveCheckBox_, &QCheckBox::toggled, savePathEdit_, &QLineEdit::setEnabled );
    connect( saveCheckBox_, &QCheckBox::toggled, browseButton_, &QPushButton::setEnabled );
    connect( sendButton_, &QPushButton::clicked, this, &PortWidget::sendCapture );
    connect( sendEdit_, &QLineEdit::returnPressed, this, &PortWidget::sendCapture );
    connect( portCombo_, &QComboBox::currentIndexChanged, this, [ this ]() { updateUiState(); } );

    // Follow the sessions, whichever view starts or stops them
    connect( sessions_, &Sessions::portsChanged, this, &PortWidget::updatePortCombo );
    connect( sessions_, &Sessions::sessionStarted, this, &PortWidget::updatePortCombo );
    connect( sessions_, &Sessions::sessionStopped, this, &PortWidget::updatePortCombo );

    // Load defaults from config
    loadDefaults();

    // The Sessions have scanned the ports already.
    updatePortCombo();
}

// ── Private slots ───────────────────────────────────────────────────────

void PortWidget::updatePortCombo()
{
    const auto currentSelection = currentPortName();
    portCombo_->clear();

    const auto& ports = sessions_->ports();
    if ( ports.isEmpty() ) {
        portCombo_->addItem( "(no ports)" );
        portCombo_->setEnabled( false );
    }
    else {
        portCombo_->setEnabled( true );
        for ( const auto& name : ports ) {
            // Mark ports that already have an active session
            if ( sessions_->isSessionActive( name ) ) {
                portCombo_->addItem( name + " ●", name );
            }
            else {
                portCombo_->addItem( name, name );
            }
        }

        // Restore previous selection if still available
        const auto idx = portCombo_->findData( currentSelection );
        if ( idx >= 0 ) {
            portCombo_->setCurrentIndex( idx );
        }
    }

    updateUiState();
}

void PortWidget::startCapture()
{
    const auto name = currentPortName();
    if ( name.isEmpty() ) {
        return;
    }

    // Don't start twice for the same port
    if ( sessions_->isSessionActive( name ) ) {
        hostLog( LOGSQUIRL_LOG_WARNING, "Serial capture already running for " + name );
        return;
    }

    // Determine save path (empty string disables saving)
    const auto savePath
        = ( saveCheckBox_->isChecked() && !savePathEdit_->text().trimmed().isEmpty() )
              ? savePathEdit_->text()
              : QString();

    sessions_->startSession( buildConfig(), savePath );
}

void PortWidget::stopCapture()
{
    sessions_->stopSession( currentPortName() );
}

void PortWidget::stopAllCaptures()
{
    sessions_->stopAll();

    hostNotify( "All serial sessions stopped." );
}

void PortWidget::browseSavePath()
{
    // An existing file is appended to, not replaced, so don't ask to replace it.
    const auto path = QFileDialog::getSaveFileName(
        this, "Save serial output", savePathEdit_->text(), "Log files (*.log *.txt);;All files (*)",
        nullptr, QFileDialog::DontConfirmOverwrite );

    if ( !path.isEmpty() ) {
        savePathEdit_->setText( path );
    }
}

void PortWidget::sendCapture()
{
    const auto text = sendEdit_->text();
    if ( text.isEmpty() ) {
        return;
    }

    const auto name = currentPortName();
    if ( name.isEmpty() || !sessions_->isSessionActive( name ) ) {
        return;
    }

    const auto lineEnding = static_cast<TxLineEnding>( lineEndingCombo_->currentData().toInt() );
    sessions_->sendToSession( name, text.toUtf8(), lineEnding );
    sendEdit_->clear();
}

// ── Private helpers ─────────────────────────────────────────────────────

void PortWidget::updateUiState()
{
    const auto name = currentPortName();
    const auto hasPort = !name.isEmpty();
    const auto isActive = hasPort && sessions_->isSessionActive( name );
    const auto activeCount = sessions_->activeSessionCount();
    const auto anyActive = activeCount > 0;

    startButton_->setEnabled( hasPort && !isActive );
    stopButton_->setEnabled( isActive );
    stopAllButton_->setEnabled( anyActive );

    // Send controls enabled when the currently selected port has an active session
    sendEdit_->setEnabled( isActive );
    sendButton_->setEnabled( isActive );
    lineEndingCombo_->setEnabled( isActive );

    if ( anyActive ) {
        statusLabel_->setText( QString( "%1 active session(s)" ).arg( activeCount ) );
    }
    else {
        statusLabel_->setText( "No active sessions" );
    }
}

QString PortWidget::currentPortName() const
{
    const auto data = portCombo_->currentData();
    return data.isValid() ? data.toString() : QString();
}

void PortWidget::loadDefaults()
{
    const auto defaults = SerialProcess::defaultConfig();
    if ( defaults.baudRate > 0 ) {
        selectBaudRate( baudCombo_, defaults.baudRate );
    }
    timestampCheckBox_->setChecked( defaults.timestamps );
}

SerialConfig PortWidget::buildConfig() const
{
    SerialConfig cfg;
    cfg.portName = currentPortName();
    cfg.baudRate = baudRateFrom( baudCombo_ );
    cfg.dataBits = static_cast<QSerialPort::DataBits>( dataBitsCombo_->currentData().toInt() );
    cfg.stopBits = static_cast<QSerialPort::StopBits>( stopBitsCombo_->currentData().toInt() );
    cfg.parity = static_cast<QSerialPort::Parity>( parityCombo_->currentData().toInt() );
    cfg.flowControl
        = static_cast<QSerialPort::FlowControl>( flowControlCombo_->currentData().toInt() );
    cfg.timestamps = timestampCheckBox_->isChecked();
    cfg.maxFileBytes = SerialProcess::defaultConfig().maxFileBytes;
    return cfg;
}

} // namespace serial_monitor
