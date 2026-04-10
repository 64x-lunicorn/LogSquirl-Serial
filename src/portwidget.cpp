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
 * ARCHITECTURE OVERVIEW
 * ─────────────────────
 * The dialog maintains a map of portName → SerialProcess*.  When the
 * user clicks "Start":
 *
 *   1. A SerialProcess is created with the configured parameters.
 *   2. SerialProcess::start() opens the serial port.
 *   3. Incoming data is streamed to a temporary file.
 *   4. The host API's open_file() is called with the temp file path
 *      and follow=true, which opens a new tail-mode tab in LogSquirl.
 *   5. The session entry is stored in sessions_.
 *
 * When the user clicks "Stop":
 *   1. SerialProcess::stop() closes the serial port.
 *   2. The session is removed from sessions_.
 *   3. The LogSquirl tab remains open (the user can close it manually).
 *
 * Multiple ports can run simultaneously — each with its own tab.
 */

#include "portwidget.h"
#include "plugin.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>

namespace serial_monitor {

// ── Construction ────────────────────────────────────────────────────────

PortWidget::PortWidget( QWidget* parent )
    : QDialog( parent )
{
    setWindowTitle( "Serial Monitor" );
    setMinimumWidth( 480 );

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
    refreshButton_->setToolTip( "Refresh port list" );
    portRow->addWidget( refreshButton_ );
    portLayout->addLayout( portRow );

    mainLayout->addWidget( portGroup );

    // ── Serial settings group ────────────────────────────────────────
    auto* settingsGroup = new QGroupBox( "Settings", this );
    auto* settingsLayout = new QFormLayout( settingsGroup );

    baudCombo_ = new QComboBox( this );
    const QList<int> baudRates = { 300, 1200, 2400, 4800, 9600, 19200,
                                   38400, 57600, 115200, 230400, 460800, 921600 };
    for ( const auto rate : baudRates ) {
        baudCombo_->addItem( QString::number( rate ), rate );
    }
    baudCombo_->setCurrentIndex( baudCombo_->findData( 115200 ) );
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

    // ── Status line ──────────────────────────────────────────────────
    statusLabel_ = new QLabel( this );
    statusLabel_->setStyleSheet( "color: gray; font-style: italic;" );
    mainLayout->addWidget( statusLabel_ );

    mainLayout->addStretch();

    // ── Connect signals ──────────────────────────────────────────────
    connect( refreshButton_, &QPushButton::clicked,
             this, &PortWidget::refreshPorts );
    connect( startButton_, &QPushButton::clicked,
             this, &PortWidget::startCapture );
    connect( stopButton_, &QPushButton::clicked,
             this, &PortWidget::stopCapture );
    connect( stopAllButton_, &QPushButton::clicked,
             this, &PortWidget::stopAllCaptures );
    connect( browseButton_, &QPushButton::clicked,
             this, &PortWidget::browseSavePath );
    connect( saveCheckBox_, &QCheckBox::toggled,
             savePathEdit_, &QLineEdit::setEnabled );
    connect( saveCheckBox_, &QCheckBox::toggled,
             browseButton_, &QPushButton::setEnabled );
    connect( portCombo_, &QComboBox::currentIndexChanged,
             this, [this]() { updateUiState(); } );

    // Load default baud rate from config
    const auto defaults = SerialProcess::defaultConfig();
    const auto baudIdx = baudCombo_->findData( defaults.baudRate );
    if ( baudIdx >= 0 ) {
        baudCombo_->setCurrentIndex( baudIdx );
    }
    timestampCheckBox_->setChecked( defaults.timestamps );

    // Initial port scan
    refreshPorts();
    updateUiState();
}

// ── Public methods ──────────────────────────────────────────────────────

void PortWidget::stopAll( bool cleanupTempFiles )
{
    const auto portNames = sessions_.keys();
    for ( const auto& name : portNames ) {
        if ( auto* proc = sessions_.value( name ) ) {
            proc->stop();
            if ( !cleanupTempFiles ) {
                proc->preserveTempFile();
            }
            proc->deleteLater();
        }
    }
    sessions_.clear();
    updateUiState();
}

int PortWidget::activeSessionCount() const
{
    return sessions_.size();
}

QStringList PortWidget::activePorts() const
{
    return sessions_.keys();
}

void PortWidget::rotateSession( const QString& portName )
{
    auto* proc = sessions_.value( portName, nullptr );
    if ( !proc || !proc->isRunning() ) {
        return;
    }

    // Prevent old temp dir from being auto-removed so the old tab keeps its data
    proc->preserveTempFile();

    const auto newPath = proc->rotateLog();
    if ( newPath.isEmpty() ) {
        if ( g_state.api && g_state.handle ) {
            g_state.api->show_notification(
                g_state.handle,
                qPrintable( "Failed to rotate log for " + portName ) );
        }
        return;
    }

    // Open the new temp file in a follow-mode tab
    if ( g_state.api && g_state.handle ) {
        g_state.api->open_file( g_state.handle, newPath.toUtf8().constData(), 1 );
        g_state.api->show_notification(
            g_state.handle,
            qPrintable( QString( "New session started for %1" ).arg( portName ) ) );
    }
}

bool PortWidget::startSession( const SerialConfig& config, const QString& savePath )
{
    const auto& name = config.portName;
    if ( name.isEmpty() || sessions_.contains( name ) ) {
        return false;
    }

    auto* proc = new SerialProcess( config, savePath, this );

    connect( proc, &SerialProcess::started, this, [this, name]() {
        hostLog( LOGSQUIRL_LOG_INFO,
                 qPrintable( "Serial session started for " + name ) );
    } );

    connect( proc, &SerialProcess::finished, this, [this, name]() {
        onSessionFinished( name );
    } );

    connect( proc, &SerialProcess::errorOccurred, this, [this, name]( const QString& msg ) {
        onSessionError( name, msg );
    } );

    proc->start();

    if ( proc->isRunning() || !proc->tempFilePath().isEmpty() ) {
        sessions_.insert( name, proc );

        if ( g_state.api && g_state.handle ) {
            const auto path = proc->tempFilePath().toUtf8();
            g_state.api->open_file( g_state.handle, path.constData(), 1 );
            g_state.api->show_notification(
                g_state.handle,
                qPrintable( QString( "Serial capture started for %1 at %2 baud" )
                                .arg( name )
                                .arg( config.baudRate ) ) );
        }

        refreshPorts();
        return true;
    }

    delete proc;
    return false;
}

void PortWidget::stopSession( const QString& portName )
{
    if ( !sessions_.contains( portName ) ) {
        return;
    }

    auto* proc = sessions_.take( portName );
    proc->stop();
    proc->preserveTempFile();

    if ( g_state.api && g_state.handle ) {
        g_state.api->show_notification(
            g_state.handle,
            qPrintable( QString( "Serial capture stopped for %1 (%2 lines)" )
                            .arg( portName )
                            .arg( proc->lineCount() ) ) );
    }

    proc->deleteLater();
    refreshPorts();
}

qint64 PortWidget::sessionLineCount( const QString& portName ) const
{
    auto* proc = sessions_.value( portName, nullptr );
    return proc ? proc->lineCount() : 0;
}

bool PortWidget::isSessionActive( const QString& portName ) const
{
    return sessions_.contains( portName );
}

// ── Private slots ───────────────────────────────────────────────────────

void PortWidget::refreshPorts()
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
            // Mark ports that already have an active session
            if ( sessions_.contains( name ) ) {
                portCombo_->addItem( name + " \u25CF", name );
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
    if ( sessions_.contains( name ) ) {
        hostLog( LOGSQUIRL_LOG_WARNING,
                 qPrintable( "Serial capture already running for " + name ) );
        return;
    }

    // Determine save path (empty string disables saving)
    const auto savePath = ( saveCheckBox_->isChecked() && !savePathEdit_->text().isEmpty() )
                              ? savePathEdit_->text()
                              : QString();

    auto config = buildConfig();
    auto* proc = new SerialProcess( config, savePath, this );

    connect( proc, &SerialProcess::started, this, [this, name]() {
        hostLog( LOGSQUIRL_LOG_INFO,
                 qPrintable( "Serial session started for " + name ) );
    } );

    connect( proc, &SerialProcess::finished, this, [this, name]() {
        onSessionFinished( name );
    } );

    connect( proc, &SerialProcess::errorOccurred, this, [this, name]( const QString& msg ) {
        onSessionError( name, msg );
    } );

    proc->start();

    if ( proc->isRunning() || !proc->tempFilePath().isEmpty() ) {
        sessions_.insert( name, proc );

        // Ask the host to open the temp file in a follow-mode tab
        if ( g_state.api && g_state.handle ) {
            const auto path = proc->tempFilePath().toUtf8();
            g_state.api->open_file( g_state.handle, path.constData(), 1 );
        }

        // Notify via host notification
        if ( g_state.api && g_state.handle ) {
            g_state.api->show_notification(
                g_state.handle,
                qPrintable( QString( "Serial capture started for %1 at %2 baud" )
                                .arg( name )
                                .arg( config.baudRate ) ) );
        }
    }
    else {
        delete proc;
    }

    refreshPorts();
}

void PortWidget::stopCapture()
{
    const auto name = currentPortName();
    if ( name.isEmpty() || !sessions_.contains( name ) ) {
        return;
    }

    auto* proc = sessions_.take( name );
    proc->stop();
    proc->preserveTempFile();
    proc->deleteLater();

    if ( g_state.api && g_state.handle ) {
        g_state.api->show_notification(
            g_state.handle,
            qPrintable( QString( "Serial capture stopped for %1 (%2 lines)" )
                            .arg( name )
                            .arg( proc->lineCount() ) ) );
    }

    refreshPorts();
}

void PortWidget::stopAllCaptures()
{
    stopAll();

    if ( g_state.api && g_state.handle ) {
        g_state.api->show_notification( g_state.handle, "All serial sessions stopped." );
    }

    refreshPorts();
}

void PortWidget::browseSavePath()
{
    const auto path = QFileDialog::getSaveFileName(
        this, "Save serial output", savePathEdit_->text(),
        "Log files (*.log *.txt);;All files (*)" );

    if ( !path.isEmpty() ) {
        savePathEdit_->setText( path );
    }
}

void PortWidget::onSessionFinished( const QString& portName )
{
    if ( sessions_.contains( portName ) ) {
        auto* proc = sessions_.take( portName );

        // Preserve the temp file so the LogSquirl tab keeps its content.
        // When using a save path the file is already persistent.
        proc->preserveTempFile();
        proc->deleteLater();

        hostLog( LOGSQUIRL_LOG_INFO,
                 qPrintable( QString( "Serial session for %1 ended." ).arg( portName ) ) );
    }

    refreshPorts();
}

void PortWidget::onSessionError( const QString& portName, const QString& message )
{
    hostLog( LOGSQUIRL_LOG_ERROR, qPrintable( portName + ": " + message ) );

    if ( g_state.api && g_state.handle ) {
        g_state.api->show_notification(
            g_state.handle,
            qPrintable( "Serial error (" + portName + "): " + message ) );
    }
}

// ── Private helpers ─────────────────────────────────────────────────────

void PortWidget::updateUiState()
{
    const auto name = currentPortName();
    const auto hasPort = !name.isEmpty();
    const auto isActive = hasPort && sessions_.contains( name );
    const auto anyActive = !sessions_.isEmpty();

    startButton_->setEnabled( hasPort && !isActive );
    stopButton_->setEnabled( isActive );
    stopAllButton_->setEnabled( anyActive );

    if ( anyActive ) {
        statusLabel_->setText(
            QString( "%1 active session(s)" ).arg( sessions_.size() ) );
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

SerialConfig PortWidget::buildConfig() const
{
    SerialConfig cfg;
    cfg.portName = currentPortName();
    cfg.baudRate = baudCombo_->currentData().toInt();
    cfg.dataBits = static_cast<QSerialPort::DataBits>( dataBitsCombo_->currentData().toInt() );
    cfg.stopBits = static_cast<QSerialPort::StopBits>( stopBitsCombo_->currentData().toInt() );
    cfg.parity = static_cast<QSerialPort::Parity>( parityCombo_->currentData().toInt() );
    cfg.flowControl
        = static_cast<QSerialPort::FlowControl>( flowControlCombo_->currentData().toInt() );
    cfg.timestamps = timestampCheckBox_->isChecked();
    return cfg;
}

} // namespace serial_monitor
