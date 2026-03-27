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
 * @file sidebarwidget.cpp
 * @brief Implementation of the sidebar panel for serial port session management.
 *
 * The sidebar panel is a compact view designed to live in the LogSquirl
 * sidebar QTabWidget.  It shows:
 *
 *   1. A port selector (combo + refresh) with serial settings.
 *   2. Start / Stop buttons.
 *   3. An active-sessions list with per-session line count, rotate,
 *      and stop buttons.
 *   4. A "Stop All" button when multiple sessions are running.
 *
 * Session logic is delegated entirely to PortWidget so that the
 * dialog and sidebar remain in sync at all times.
 */

#include "sidebarwidget.h"
#include "portwidget.h"
#include "plugin.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>

namespace serial_monitor {

// ── Construction ────────────────────────────────────────────────────────

SidebarWidget::SidebarWidget( PortWidget* portWidget, QWidget* parent )
    : QWidget( parent )
    , portWidget_( portWidget )
{
    auto* mainLayout = new QVBoxLayout( this );
    mainLayout->setContentsMargins( 4, 4, 4, 4 );
    mainLayout->setSpacing( 6 );

    // ── Port selector ────────────────────────────────────────────────
    auto* portGroup = new QGroupBox( "Port", this );
    auto* portLayout = new QVBoxLayout( portGroup );
    portLayout->setContentsMargins( 6, 6, 6, 6 );

    auto* portRow = new QHBoxLayout();
    portCombo_ = new QComboBox( this );
    portCombo_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
    portCombo_->setToolTip( "Select a serial port" );
    portRow->addWidget( portCombo_ );

    refreshButton_ = new QPushButton( "\u27F3", this );
    refreshButton_->setFixedWidth( 30 );
    refreshButton_->setToolTip( "Refresh port list" );
    portRow->addWidget( refreshButton_ );
    portLayout->addLayout( portRow );

    mainLayout->addWidget( portGroup );

    // ── Serial settings ──────────────────────────────────────────────
    auto* settingsGroup = new QGroupBox( "Settings", this );
    auto* settingsLayout = new QFormLayout( settingsGroup );
    settingsLayout->setContentsMargins( 6, 6, 6, 6 );

    baudCombo_ = new QComboBox( this );
    const QList<int> baudRates = { 300, 1200, 2400, 4800, 9600, 19200,
                                   38400, 57600, 115200, 230400, 460800, 921600 };
    for ( const auto rate : baudRates ) {
        baudCombo_->addItem( QString::number( rate ), rate );
    }
    baudCombo_->setCurrentIndex( baudCombo_->findData( 115200 ) );
    settingsLayout->addRow( "Baud:", baudCombo_ );

    dataBitsCombo_ = new QComboBox( this );
    dataBitsCombo_->addItem( "5", QSerialPort::Data5 );
    dataBitsCombo_->addItem( "6", QSerialPort::Data6 );
    dataBitsCombo_->addItem( "7", QSerialPort::Data7 );
    dataBitsCombo_->addItem( "8", QSerialPort::Data8 );
    dataBitsCombo_->setCurrentIndex( 3 );
    settingsLayout->addRow( "Data:", dataBitsCombo_ );

    stopBitsCombo_ = new QComboBox( this );
    stopBitsCombo_->addItem( "1", QSerialPort::OneStop );
    stopBitsCombo_->addItem( "1.5", QSerialPort::OneAndHalfStop );
    stopBitsCombo_->addItem( "2", QSerialPort::TwoStop );
    stopBitsCombo_->setCurrentIndex( 0 );
    settingsLayout->addRow( "Stop:", stopBitsCombo_ );

    parityCombo_ = new QComboBox( this );
    parityCombo_->addItem( "None", QSerialPort::NoParity );
    parityCombo_->addItem( "Even", QSerialPort::EvenParity );
    parityCombo_->addItem( "Odd", QSerialPort::OddParity );
    parityCombo_->addItem( "Mark", QSerialPort::MarkParity );
    parityCombo_->addItem( "Space", QSerialPort::SpaceParity );
    parityCombo_->setCurrentIndex( 0 );
    settingsLayout->addRow( "Parity:", parityCombo_ );

    flowControlCombo_ = new QComboBox( this );
    flowControlCombo_->addItem( "None", QSerialPort::NoFlowControl );
    flowControlCombo_->addItem( "RTS/CTS", QSerialPort::HardwareControl );
    flowControlCombo_->addItem( "XON/XOFF", QSerialPort::SoftwareControl );
    flowControlCombo_->setCurrentIndex( 0 );
    settingsLayout->addRow( "Flow:", flowControlCombo_ );

    timestampCheckBox_ = new QCheckBox( "Timestamps", this );
    timestampCheckBox_->setChecked( true );
    timestampCheckBox_->setToolTip(
        "Add [YYYY-MM-DD HH:mm:ss.zzz] prefix to each line" );
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
    mainLayout->addLayout( buttonRow );

    // ── Log directory ────────────────────────────────────────────────
    auto* logDirGroup = new QGroupBox( "Log Directory", this );
    auto* logDirLayout = new QHBoxLayout( logDirGroup );
    logDirLayout->setContentsMargins( 6, 6, 6, 6 );

    logDirEdit_ = new QLineEdit( this );
    logDirEdit_->setPlaceholderText( "(logs saved to temp directory)" );
    logDirEdit_->setToolTip(
        "Directory where log files are saved.\n"
        "Files are named: YYYY-MM-dd_HHmmss_<port>.log" );
    logDirLayout->addWidget( logDirEdit_ );

    logDirBrowseButton_ = new QPushButton( "\u2026", this );
    logDirBrowseButton_->setFixedWidth( 30 );
    logDirBrowseButton_->setToolTip( "Browse for log directory" );
    logDirLayout->addWidget( logDirBrowseButton_ );

    mainLayout->addWidget( logDirGroup );

    // ── Active sessions ──────────────────────────────────────────────
    auto* sessionsGroup = new QGroupBox( "Active Sessions", this );
    auto* sessionsLayout = new QVBoxLayout( sessionsGroup );
    sessionsLayout->setContentsMargins( 6, 6, 6, 6 );

    sessionList_ = new QListWidget( this );
    sessionList_->setAlternatingRowColors( true );
    sessionList_->setSelectionMode( QAbstractItemView::NoSelection );
    sessionsLayout->addWidget( sessionList_ );

    stopAllButton_ = new QPushButton( "\u25A0 Stop All", this );
    stopAllButton_->setToolTip( "Stop all active serial sessions" );
    sessionsLayout->addWidget( stopAllButton_ );

    mainLayout->addWidget( sessionsGroup );

    // ── Status ───────────────────────────────────────────────────────
    statusLabel_ = new QLabel( this );
    statusLabel_->setStyleSheet( "color: gray; font-style: italic;" );
    mainLayout->addWidget( statusLabel_ );

    mainLayout->addStretch();

    // ── Connections ──────────────────────────────────────────────────
    connect( refreshButton_, &QPushButton::clicked,
             this, &SidebarWidget::refreshPorts );
    connect( startButton_, &QPushButton::clicked,
             this, &SidebarWidget::startCapture );
    connect( stopButton_, &QPushButton::clicked,
             this, &SidebarWidget::stopSelectedCapture );
    connect( stopAllButton_, &QPushButton::clicked,
             this, &SidebarWidget::stopAllCaptures );
    connect( logDirBrowseButton_, &QPushButton::clicked, this, [this]() {
        const auto dir = QFileDialog::getExistingDirectory(
            this, "Select Log Directory", logDirEdit_->text() );
        if ( !dir.isEmpty() ) {
            logDirEdit_->setText( dir );
            saveLogDir();
        }
    } );
    connect( logDirEdit_, &QLineEdit::editingFinished,
             this, &SidebarWidget::saveLogDir );
    connect( portCombo_, &QComboBox::currentIndexChanged,
             this, [this]() { updateUiState(); } );

    // Load defaults from config
    const auto defaults = SerialProcess::defaultConfig();
    const auto baudIdx = baudCombo_->findData( defaults.baudRate );
    if ( baudIdx >= 0 ) {
        baudCombo_->setCurrentIndex( baudIdx );
    }
    timestampCheckBox_->setChecked( defaults.timestamps );

    // Periodic refresh of line counts (every 1 second)
    refreshTimer_ = new QTimer( this );
    refreshTimer_->setInterval( 1000 );
    connect( refreshTimer_, &QTimer::timeout,
             this, &SidebarWidget::refreshSessionList );
    refreshTimer_->start();

    // Initial populate
    loadLogDir();
    refreshPorts();
    updateUiState();
}

// ── Private slots ───────────────────────────────────────────────────────

void SidebarWidget::refreshPorts()
{
    const auto currentSelection = currentPortName();
    portCombo_->clear();

    const auto ports = SerialProcess::discoverPorts();
    if ( ports.isEmpty() ) {
        portCombo_->addItem( "(no ports)" );
        portCombo_->setEnabled( false );
    }
    else {
        portCombo_->setEnabled( true );
        for ( const auto& name : ports ) {
            if ( portWidget_->isSessionActive( name ) ) {
                portCombo_->addItem( name + " \u25CF", name );
            }
            else {
                portCombo_->addItem( name, name );
            }
        }

        const auto idx = portCombo_->findData( currentSelection );
        if ( idx >= 0 ) {
            portCombo_->setCurrentIndex( idx );
        }
    }

    rebuildSessionList();
    updateUiState();
}

void SidebarWidget::startCapture()
{
    const auto name = currentPortName();
    if ( name.isEmpty() ) {
        return;
    }

    const auto config = buildConfig();
    const auto savePath = generateSavePath( name );
    portWidget_->startSession( config, savePath );
    refreshPorts();
}

void SidebarWidget::stopSelectedCapture()
{
    const auto name = currentPortName();
    if ( name.isEmpty() ) {
        return;
    }

    portWidget_->stopSession( name );
    refreshPorts();
}

void SidebarWidget::stopAllCaptures()
{
    portWidget_->stopAll();

    if ( g_state.api && g_state.handle ) {
        g_state.api->show_notification( g_state.handle,
                                        "All serial sessions stopped." );
    }

    refreshPorts();
}

void SidebarWidget::refreshSessionList()
{
    const int currentCount = portWidget_->activeSessionCount();
    if ( sessionList_->count() != currentCount ) {
        rebuildSessionList();
        updateUiState();
        return;
    }

    // Names don't change — nothing to refresh for existing items
}

// ── Private helpers ─────────────────────────────────────────────────────

void SidebarWidget::rebuildSessionList()
{
    sessionList_->clear();

    const auto ports = portWidget_->activePorts();
    for ( const auto& portName : ports ) {
        auto* item = new QListWidgetItem();
        sessionList_->addItem( item );

        // Custom widget row with label + Rotate + Stop
        auto* row = new QWidget( sessionList_ );
        auto* rowLayout = new QHBoxLayout( row );
        rowLayout->setContentsMargins( 2, 2, 2, 2 );
        rowLayout->setSpacing( 4 );

        auto* label = new QLabel( portName, row );
        label->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Preferred );
        rowLayout->addWidget( label );

        auto* rotateBtn = new QPushButton( "\u21BB", row );
        rotateBtn->setFixedSize( 26, 26 );
        rotateBtn->setToolTip( "Rotate log (start new session)" );
        rowLayout->addWidget( rotateBtn );

        auto* stopBtn = new QPushButton( "\u25A0", row );
        stopBtn->setFixedSize( 26, 26 );
        stopBtn->setToolTip( "Stop this session" );
        rowLayout->addWidget( stopBtn );

        connect( rotateBtn, &QPushButton::clicked, this,
                 [this, portName]() {
                     portWidget_->rotateSession( portName );
                     refreshPorts();
                 } );

        connect( stopBtn, &QPushButton::clicked, this,
                 [this, portName]() {
                     portWidget_->stopSession( portName );
                     refreshPorts();
                 } );

        item->setSizeHint( row->sizeHint() );
        sessionList_->setItemWidget( item, row );
    }
}

QString SidebarWidget::currentPortName() const
{
    const auto data = portCombo_->currentData();
    return data.isValid() ? data.toString() : QString();
}

SerialConfig SidebarWidget::buildConfig() const
{
    SerialConfig config;
    config.portName = currentPortName();
    config.baudRate = baudCombo_->currentData().toInt();
    config.dataBits = static_cast<QSerialPort::DataBits>(
        dataBitsCombo_->currentData().toInt() );
    config.stopBits = static_cast<QSerialPort::StopBits>(
        stopBitsCombo_->currentData().toInt() );
    config.parity = static_cast<QSerialPort::Parity>(
        parityCombo_->currentData().toInt() );
    config.flowControl = static_cast<QSerialPort::FlowControl>(
        flowControlCombo_->currentData().toInt() );
    config.timestamps = timestampCheckBox_->isChecked();
    return config;
}

void SidebarWidget::updateUiState()
{
    const auto name = currentPortName();
    const bool hasPort = !name.isEmpty();
    const bool isActive = hasPort && portWidget_->isSessionActive( name );
    const int activeCount = portWidget_->activeSessionCount();

    startButton_->setEnabled( hasPort && !isActive );
    stopButton_->setEnabled( isActive );
    stopAllButton_->setEnabled( activeCount > 0 );
    stopAllButton_->setVisible( activeCount > 1 );

    if ( activeCount == 0 ) {
        statusLabel_->setText( {} );
    }
    else {
        statusLabel_->setText(
            QString( "%1 active session(s)" ).arg( activeCount ) );
    }
}

QString SidebarWidget::generateSavePath( const QString& portName ) const
{
    const auto dir = logDirEdit_->text().trimmed();
    if ( dir.isEmpty() ) {
        return {};
    }

    // Ensure the directory exists
    QDir().mkpath( dir );

    // Format: YYYY-MM-dd_HHmmss_<port>.log
    const auto timestamp = QDateTime::currentDateTime().toString( "yyyy-MM-dd_HHmmss" );
    // Sanitise the port name for use as a filename component
    auto safeName = portName;
    safeName.replace( QRegularExpression( "[^a-zA-Z0-9._-]" ), "_" );
    return QDir( dir ).filePath(
        QString( "%1_%2.log" ).arg( timestamp, safeName ) );
}

void SidebarWidget::loadLogDir()
{
    const auto cfgDir = SerialProcess::configDir();
    if ( cfgDir.isEmpty() ) {
        return;
    }
    QSettings settings( cfgDir + "/serial.ini", QSettings::IniFormat );
    logDirEdit_->setText( settings.value( "serial/logDir" ).toString() );
}

void SidebarWidget::saveLogDir()
{
    const auto cfgDir = SerialProcess::configDir();
    if ( cfgDir.isEmpty() ) {
        return;
    }
    QSettings settings( cfgDir + "/serial.ini", QSettings::IniFormat );
    settings.setValue( "serial/logDir", logDirEdit_->text().trimmed() );
}

} // namespace serial_monitor
