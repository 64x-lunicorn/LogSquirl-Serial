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
 * Session logic is in the Sessions, of which both the dialog and the
 * sidebar are views: each updates on the Sessions' signals, so both stay
 * in sync whichever started or stopped a session, and when one ends on its
 * own.
 */

#include "sidebarwidget.h"
#include "baudrate.h"
#include "plugin.h"
#include "sessions.h"

#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QSettings>
#include <QVBoxLayout>

namespace serial_monitor {

namespace {

/**
 * Where captures go while the log directory field is empty: `logs` in the
 * plugin's configuration directory, where they are kept.  Empty without a
 * configuration directory: each session then writes to a temporary file,
 * removed when LogSquirl quits.
 */
QString defaultLogDir()
{
    const auto cfgDir = SerialProcess::configDir();
    return cfgDir.isEmpty() ? QString() : cfgDir + "/logs";
}

} // namespace

// ── Construction ────────────────────────────────────────────────────────

SidebarWidget::SidebarWidget( Sessions* sessions, QWidget* parent )
    : QWidget( parent )
    , sessions_( sessions )
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
    refreshButton_->setObjectName( "refresh" );
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
    initBaudRateCombo( baudCombo_ );
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
    timestampCheckBox_->setObjectName( "timestamps" );
    timestampCheckBox_->setChecked( true );
    timestampCheckBox_->setToolTip( "Add [YYYY-MM-DD HH:mm:ss.zzz] prefix to each line" );
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

    // ── Send command ─────────────────────────────────────────────────
    auto* sendGroup = new QGroupBox( "Send", this );
    auto* sendLayout = new QVBoxLayout( sendGroup );
    sendLayout->setContentsMargins( 6, 6, 6, 6 );

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
    lineEndingCombo_->addItem( "CRLF (\\r\\n)",
                               static_cast<int>( serial_monitor::TxLineEnding::CRLF ) );
    lineEndingCombo_->addItem( "LF (\\n)", static_cast<int>( serial_monitor::TxLineEnding::LF ) );
    lineEndingCombo_->addItem( "CR (\\r)", static_cast<int>( serial_monitor::TxLineEnding::CR ) );
    lineEndingCombo_->addItem( "None", static_cast<int>( serial_monitor::TxLineEnding::None ) );
    lineEndingCombo_->setCurrentIndex( 0 );
    lineEndingRow->addWidget( lineEndingCombo_ );
    sendLayout->addLayout( lineEndingRow );

    mainLayout->addWidget( sendGroup );

    // ── Log directory ────────────────────────────────────────────────
    auto* logDirGroup = new QGroupBox( "Log Directory", this );
    auto* logDirLayout = new QHBoxLayout( logDirGroup );
    logDirLayout->setContentsMargins( 6, 6, 6, 6 );

    logDirEdit_ = new QLineEdit( this );
    logDirEdit_->setObjectName( "logDir" );
    const auto defaultDir = defaultLogDir();
    const auto whenEmpty = defaultDir.isEmpty()
                               ? QString( "temporary files, removed when LogSquirl quits" )
                               : QDir::toNativeSeparators( defaultDir );
    logDirEdit_->setPlaceholderText( "(empty: " + whenEmpty + ")" );
    logDirEdit_->setToolTip( QString( "Directory where log files are saved and kept.\n"
                                      "When empty: %1.\n"
                                      "Files are named: YYYY-MM-dd_HHmmss_<port>.log" )
                                 .arg( whenEmpty ) );
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
    connect( refreshButton_, &QPushButton::clicked, sessions_, &Sessions::refreshPorts );
    connect( startButton_, &QPushButton::clicked, this, &SidebarWidget::startCapture );
    connect( stopButton_, &QPushButton::clicked, this, &SidebarWidget::stopSelectedCapture );
    connect( stopAllButton_, &QPushButton::clicked, this, &SidebarWidget::stopAllCaptures );
    connect( sendButton_, &QPushButton::clicked, this, &SidebarWidget::sendCommand );
    connect( sendEdit_, &QLineEdit::returnPressed, this, &SidebarWidget::sendCommand );
    connect( logDirBrowseButton_, &QPushButton::clicked, this, [ this ]() {
        const auto dir = QFileDialog::getExistingDirectory( this, "Select Log Directory",
                                                            logDirEdit_->text() );
        if ( !dir.isEmpty() ) {
            logDirEdit_->setText( dir );
            saveLogDir();
        }
    } );
    connect( logDirEdit_, &QLineEdit::editingFinished, this, &SidebarWidget::saveLogDir );
    connect( portCombo_, &QComboBox::currentIndexChanged, this, [ this ]() { updateUiState(); } );

    // Load defaults from config
    loadDefaults();

    // Follow the sessions, whichever view starts or stops them
    connect( sessions_, &Sessions::portsChanged, this, &SidebarWidget::updatePortList );
    connect( sessions_, &Sessions::sessionStarted, this, &SidebarWidget::updatePortList );
    connect( sessions_, &Sessions::sessionStopped, this, &SidebarWidget::updatePortList );

    // Initial populate.  The Sessions have scanned the ports already.
    loadLogDir();
    updatePortList();
}

// ── Private slots ───────────────────────────────────────────────────────

void SidebarWidget::updatePortList()
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
            if ( sessions_->isSessionActive( name ) ) {
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
    sessions_->startSession( config, savePath );
}

void SidebarWidget::stopSelectedCapture()
{
    const auto name = currentPortName();
    if ( name.isEmpty() ) {
        return;
    }

    sessions_->stopSession( name );
}

void SidebarWidget::stopAllCaptures()
{
    sessions_->stopAll();

    hostNotify( "All serial sessions stopped." );
}

void SidebarWidget::sendCommand()
{
    const auto text = sendEdit_->text();
    if ( text.isEmpty() ) {
        return;
    }

    // Only ever send to the selected port.  Another device may react to
    // the command in ways nobody intended.
    const auto name = currentPortName();
    if ( !sessions_->isSessionActive( name ) ) {
        hostNotify(
            name.isEmpty()
                ? QString( "Select a port with a running capture to send a command." )
                : QString( "No capture is running on %1; the command was not sent." ).arg( name ) );
        return;
    }

    const auto lineEnding = static_cast<TxLineEnding>( lineEndingCombo_->currentData().toInt() );
    sessions_->sendToSession( name, text.toUtf8(), lineEnding );
    sendEdit_->clear();
}

// ── Private helpers ─────────────────────────────────────────────────────

void SidebarWidget::rebuildSessionList()
{
    sessionList_->clear();

    const auto ports = sessions_->activePorts();
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
                 [ this, portName ]() { sessions_->rotateSession( portName ); } );

        connect( stopBtn, &QPushButton::clicked, this,
                 [ this, portName ]() { sessions_->stopSession( portName ); } );

        item->setSizeHint( row->sizeHint() );
        sessionList_->setItemWidget( item, row );
    }
}

QString SidebarWidget::currentPortName() const
{
    const auto data = portCombo_->currentData();
    return data.isValid() ? data.toString() : QString();
}

void SidebarWidget::loadDefaults()
{
    const auto defaults = SerialProcess::defaultConfig();
    if ( defaults.baudRate > 0 ) {
        selectBaudRate( baudCombo_, defaults.baudRate );
    }
    timestampCheckBox_->setChecked( defaults.timestamps );
}

SerialConfig SidebarWidget::buildConfig() const
{
    SerialConfig config;
    config.portName = currentPortName();
    config.baudRate = baudRateFrom( baudCombo_ );
    config.dataBits = static_cast<QSerialPort::DataBits>( dataBitsCombo_->currentData().toInt() );
    config.stopBits = static_cast<QSerialPort::StopBits>( stopBitsCombo_->currentData().toInt() );
    config.parity = static_cast<QSerialPort::Parity>( parityCombo_->currentData().toInt() );
    config.flowControl
        = static_cast<QSerialPort::FlowControl>( flowControlCombo_->currentData().toInt() );
    config.timestamps = timestampCheckBox_->isChecked();
    config.txLineEnding = static_cast<TxLineEnding>( lineEndingCombo_->currentData().toInt() );
    config.maxFileBytes = SerialProcess::defaultConfig().maxFileBytes;
    return config;
}

void SidebarWidget::updateUiState()
{
    const auto name = currentPortName();
    const bool hasPort = !name.isEmpty();
    const bool isActive = hasPort && sessions_->isSessionActive( name );
    const int activeCount = sessions_->activeSessionCount();

    startButton_->setEnabled( hasPort && !isActive );
    stopButton_->setEnabled( isActive );
    stopAllButton_->setEnabled( activeCount > 0 );
    stopAllButton_->setVisible( activeCount > 1 );

    // Send controls enabled when the currently selected port has an active session
    sendEdit_->setEnabled( isActive );
    sendButton_->setEnabled( isActive );
    lineEndingCombo_->setEnabled( isActive );

    if ( activeCount == 0 ) {
        statusLabel_->setText( {} );
    }
    else {
        statusLabel_->setText( QString( "%1 active session(s)" ).arg( activeCount ) );
    }
}

QString SidebarWidget::generateSavePath( const QString& portName ) const
{
    auto dir = logDirEdit_->text().trimmed();
    if ( dir.isEmpty() ) {
        // Default to <configDir>/logs/ so that captures are always persisted;
        // without a configuration directory, to a temporary file.
        dir = defaultLogDir();
        if ( dir.isEmpty() ) {
            return {};
        }
    }

    // Ensure the directory exists.  A relative one is left as it is:
    // SerialProcess::start() refuses it with a message, rather than create
    // it in LogSquirl's working directory.
    if ( const auto absolute = SerialProcess::absoluteUserPath( dir ); !absolute.isEmpty() ) {
        dir = absolute;
        QDir().mkpath( dir );
    }

    // Format: YYYY-MM-dd_HHmmss_<port>.log
    return SerialProcess::generateLogPath( dir, portName );
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
    settings.sync();
    const bool failed = settings.status() != QSettings::NoError;
    // Told once, not on every edit, until a save works again.
    if ( failed && !logDirSaveFailed_ ) {
        const auto message = QString( "The Serial Monitor log directory could not be saved to %1; "
                                      "it is used until LogSquirl quits." )
                                 .arg( settings.fileName() );
        hostLog( LOGSQUIRL_LOG_WARNING, message );
        hostNotify( message );
    }
    logDirSaveFailed_ = failed;
}

} // namespace serial_monitor
