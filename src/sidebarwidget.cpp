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
 * @brief Implementation of the sidebar tab.
 */

#include "sidebarwidget.h"

#include "connectiontile.h"
#include "host.h"
#include "profiles.h"
#include "sessions.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace serial_monitor {

namespace {

/// How often the running captures' counters are read.
constexpr int kRefreshMs = 1000;

/// What @p profile is, for its tile: its port, then its frame and flow
/// control.
QStringList summaryOf( const PortProfile& profile )
{
    return {
        profile.serial.portName,
        QStringLiteral( "%1 · %2" )
            .arg( frameOf( profile.serial ), flowControlName( profile.serial.flowControl ) ),
    };
}

/// @p bytes as a size: "14 bytes", "1.5 KiB", "1.0 GiB".
QString sizeText( qint64 bytes )
{
    return QLocale().formattedDataSize( bytes, 1, QLocale::DataSizeIecFormat );
}

/// @p count, then @p one or @p many: "1 line", "2 lines".
QString countText( qint64 count, const char* one, const char* many )
{
    return QStringLiteral( "%1 %2" ).arg( QLocale().toString( count ),
                                          QLatin1String( count == 1 ? one : many ) );
}

/// The time since @p start as h:mm:ss.
QString runningTime( const QDateTime& start )
{
    const auto seconds = std::max<qint64>( start.secsTo( QDateTime::currentDateTime() ), 0 );
    return QStringLiteral( "%1:%2:%3" )
        .arg( seconds / 3600 )
        .arg( seconds / 60 % 60, 2, 10, QLatin1Char( '0' ) )
        .arg( seconds % 60, 2, 10, QLatin1Char( '0' ) );
}

/// What the running @p session has done, for its tile: its rate
/// (@p linesPerSecond) and running time, its lines and bytes, and its file.
QStringList detailsOf( const SerialProcess& session, qint64 linesPerSecond )
{
    const auto limit = session.nextRotationSize();
    return {
        QStringLiteral( "running · %1 lines/s · %2" )
            .arg( QLocale().toString( linesPerSecond ), runningTime( session.startTime() ) ),
        QStringLiteral( "%1 · %2 received" )
            .arg( countText( session.totalLineCount(), "line", "lines" ),
                  sizeText( session.receivedBytes() ) ),
        QStringLiteral( "file %1 · %2 · %3" )
            .arg( sizeText( session.fileSize() ),
                  limit > 0 ? QStringLiteral( "new file at %1" ).arg( sizeText( limit ) )
                            : QStringLiteral( "no size limit" ),
                  countText( session.rotationCount(), "rotation", "rotations" ) ),
    };
}

} // namespace

SidebarWidget::SidebarWidget( Host& host, ProfileStore* profiles, Sessions* sessions,
                              QWidget* parent )
    : QWidget( parent )
    , host_( host )
    , profiles_( profiles )
    , sessions_( sessions )
    , stopAll_( new QPushButton( QStringLiteral( "■ Stop all" ), this ) )
    , empty_( new QLabel(
          QStringLiteral( "No saved configuration yet: New configuration… adds one." ), this ) )
    , tiles_( new QVBoxLayout )
    , timer_( new QTimer( this ) )
{
    auto* newConfiguration = new QPushButton( QStringLiteral( "New configuration…" ), this );
    newConfiguration->setObjectName( "newConfiguration" );
    newConfiguration->setToolTip(
        QStringLiteral( "Set up a serial port and its settings, and save them as a tile" ) );
    stopAll_->setObjectName( "stopAll" );
    stopAll_->setToolTip( QStringLiteral( "Stop every capture; their tabs keep the files" ) );
    empty_->setObjectName( "empty" );
    empty_->setWordWrap( true );

    auto* buttons = new QHBoxLayout;
    buttons->addWidget( newConfiguration );
    buttons->addStretch();
    buttons->addWidget( stopAll_ );

    // The tiles scroll, the buttons stay.
    auto* content = new QWidget;
    auto* contentLayout = new QVBoxLayout( content );
    contentLayout->setContentsMargins( 0, 0, 0, 0 );
    contentLayout->setSpacing( 8 );
    tiles_->setSpacing( 8 );
    contentLayout->addWidget( empty_ );
    contentLayout->addLayout( tiles_ );
    contentLayout->addStretch();
    auto* scroll = new QScrollArea( this );
    scroll->setWidgetResizable( true );
    scroll->setFrameShape( QFrame::NoFrame );
    scroll->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    scroll->setWidget( content );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 4, 4, 4, 4 );
    layout->addLayout( buttons );
    layout->addWidget( scroll, 1 );

    connect( newConfiguration, &QPushButton::clicked, this,
             &SidebarWidget::newConfigurationRequested );
    // Every slot runs in the host's event loop: nothing may throw into it.
    connect( stopAll_, &QPushButton::clicked, this, [ this ] {
        guarded( host_, "stopping every serial capture", [ this ] {
            sessions_->stopAll();
            host_.notify( QStringLiteral( "All serial sessions stopped." ) );
        } );
    } );
    const auto follow = [ this ] {
        guarded( host_, "showing the serial captures", [ this ] { updateTiles(); } );
    };
    connect( sessions_, &Sessions::sessionStarted, this, follow );
    connect( sessions_, &Sessions::sessionStopped, this, follow );
    connect( sessions_, &Sessions::sessionChanged, this, follow ); // rotated
    connect( profiles_, &ProfileStore::changed, this, [ this ] {
        guarded( host_, "showing the serial configurations", [ this ] { reload(); } );
    } );
    connect( timer_, &QTimer::timeout, this, [ this ] {
        guarded( host_, "counting the serial captures", [ this ] { refresh(); } );
    } );
    timer_->setInterval( kRefreshMs );

    reload();
}

ConnectionTile* SidebarWidget::tile( const QString& name ) const
{
    return tileByName_.value( name );
}

void SidebarWidget::refresh()
{
    // A stopped tile changes with a session's start or stop, which
    // updateTiles() shows; only the running ones count.
    for ( auto* tile : std::as_const( tileByName_ ) ) {
        const auto* capture = sessions_->captureOf( tile->name() );
        if ( !capture ) {
            continue;
        }
        if ( const auto sample = samples_.find( tile->name() ); sample != samples_.end() ) {
            const auto lines = capture->totalLineCount();
            const auto elapsedMs = std::max<qint64>( sample->since.restart(), 1 );
            sample->rate = std::max<qint64>( lines - sample->lines, 0 ) * 1000 / elapsedMs;
            sample->lines = lines;
        }
        showRunning( tile, *capture );
    }
}

QWidget* SidebarWidget::controlsFor( const QString& name )
{
    auto* controls = new QWidget;
    controls->setObjectName( "controls" );

    auto* command = new QLineEdit( controls );
    command->setObjectName( "command" );
    command->setPlaceholderText( QStringLiteral( "Command to send…" ) );
    command->setToolTip( QStringLiteral( "A command for the device on this port" ) );
    command->setClearButtonEnabled( true );
    command->setText( commandByName_.value( name ) );
    auto* send = new QPushButton( QStringLiteral( "➤ Send" ), controls );
    send->setObjectName( "send" );
    send->setToolTip(
        QStringLiteral( "Send the command, with the line ending, to this port; the log shows it "
                        "as a [TX] line" ) );

    auto* lineEnding = new QComboBox( controls );
    lineEnding->setObjectName( "lineEnding" );
    lineEnding->setToolTip( QStringLiteral( "What ends the command sent" ) );
    lineEnding->addItem( QStringLiteral( "CRLF (\\r\\n)" ),
                         static_cast<int>( TxLineEnding::CRLF ) );
    lineEnding->addItem( QStringLiteral( "LF (\\n)" ), static_cast<int>( TxLineEnding::LF ) );
    lineEnding->addItem( QStringLiteral( "CR (\\r)" ), static_cast<int>( TxLineEnding::CR ) );
    lineEnding->addItem( QStringLiteral( "None" ), static_cast<int>( TxLineEnding::None ) );
    lineEnding->setCurrentIndex(
        std::max( lineEnding->findData(
                      static_cast<int>( lineEndingByName_.value( name, TxLineEnding::CRLF ) ) ),
                  0 ) );
    auto* rotateButton = new QPushButton( QStringLiteral( "↻ Rotate" ), controls );
    rotateButton->setObjectName( "rotate" );
    rotateButton->setToolTip( QStringLiteral(
        "Go on in a new log file, opened in a new tab; the old tab keeps its file" ) );

    auto* sendRow = new QHBoxLayout;
    sendRow->addWidget( command, 1 );
    sendRow->addWidget( send );
    auto* optionsRow = new QHBoxLayout;
    optionsRow->addWidget( new QLabel( QStringLiteral( "Line ending:" ), controls ) );
    optionsRow->addWidget( lineEnding );
    optionsRow->addStretch();
    optionsRow->addWidget( rotateButton );
    auto* layout = new QVBoxLayout( controls );
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->setSpacing( 6 );
    layout->addLayout( sendRow );
    layout->addLayout( optionsRow );

    const auto sendCommand = [ this, name, command, lineEnding ] {
        guarded( host_, "sending to a serial port", [ this, &name, command, lineEnding ] {
            sendFrom( name, command,
                      static_cast<TxLineEnding>( lineEnding->currentData().toInt() ) );
        } );
    };
    connect( send, &QPushButton::clicked, this, sendCommand );
    connect( command, &QLineEdit::returnPressed, this, sendCommand );
    connect( lineEnding, &QComboBox::currentIndexChanged, this, [ this, name, lineEnding ] {
        guarded( host_, "choosing a line ending", [ this, &name, lineEnding ] {
            lineEndingByName_.insert(
                name, static_cast<TxLineEnding>( lineEnding->currentData().toInt() ) );
        } );
    } );
    // Typed, sent (emptied) or cleared: what a rebuilt tile shows again.
    connect( command, &QLineEdit::textChanged, this, [ this, name ]( const QString& text ) {
        guarded( host_, "typing a serial command", [ this, &name, &text ] {
            if ( text.isEmpty() ) {
                commandByName_.remove( name );
            }
            else {
                commandByName_.insert( name, text );
            }
        } );
    } );
    // sessionChanged() shows the new file on the tile.
    connect( rotateButton, &QPushButton::clicked, this, [ this, name ] {
        guarded( host_, "rotating a serial capture's log",
                 [ this, &name ] { sessions_->rotate( name ); } );
    } );
    return controls;
}

void SidebarWidget::sendFrom( const QString& name, QLineEdit* command, TxLineEnding lineEnding )
{
    const auto text = command->text();
    if ( text.isEmpty() ) {
        return;
    }
    // A failure has been told by a notification, and a capture that has
    // ended takes nothing; the command stays, to be sent again.
    if ( sessions_->send( name, text.toUtf8(), lineEnding ) ) {
        command->clear();
    }
    auto* tile = tileByName_.value( name );
    if ( const auto* capture = sessions_->captureOf( name ); tile && capture ) {
        showRunning( tile, *capture ); // the [TX] line counts
    }
}

void SidebarWidget::reload()
{
    // Later: a tile's own click may have led here.
    for ( auto* tile : std::as_const( tileByName_ ) ) {
        tile->setParent( nullptr );
        tile->deleteLater();
    }
    tileByName_.clear();

    const auto profiles = profiles_->list();
    for ( const auto& profile : profiles ) {
        auto* tile = new ConnectionTile( profile.name, this );
        tile->setSummary( summaryOf( profile ) );
        const auto name = profile.name;
        tile->setControls( controlsFor( name ) );
        connect( tile, &ConnectionTile::startClicked, this, [ this, name ] {
            guarded( host_, "starting a serial capture", [ this, &name ] { start( name ); } );
        } );
        // sessionStopped() shows it stopped; a capture that has ended
        // meanwhile has shown so already.
        connect( tile, &ConnectionTile::stopClicked, this, [ this, name ] {
            guarded( host_, "stopping a serial capture",
                     [ this, &name ] { sessions_->stop( name ); } );
        } );
        connect( tile, &ConnectionTile::editClicked, this, [ this, name ] {
            guarded( host_, "editing a serial configuration",
                     [ this, &name ] { Q_EMIT editRequested( name ); } );
        } );
        connect( tile, &ConnectionTile::deleteClicked, this, [ this, name ] {
            guarded( host_, "deleting a serial configuration",
                     [ this, &name ] { confirmDelete( name ); } );
        } );
        tiles_->addWidget( tile );
        tileByName_.insert( name, tile );
    }
    empty_->setVisible( profiles.isEmpty() );
    updateTiles();
}

void SidebarWidget::start( const QString& name )
{
    const auto profile = profiles_->find( name );
    if ( !profile ) {
        reload(); // deleted meanwhile
        return;
    }
    // A refusal (the configuration incomplete, its port captured already,
    // its port or file not opening) the sessions have told the user, once;
    // sessionStarted() updates the tile when it starts.
    sessions_->start( *profile, profiles_->defaults(), profiles_->defaultLogDir() );
}

void SidebarWidget::confirmDelete( const QString& name )
{
    const auto answer
        = QMessageBox::question( this, QStringLiteral( "Serial Monitor" ),
                                 QStringLiteral( "Delete the saved configuration %1?" ).arg( name ),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel );
    if ( answer != QMessageBox::Yes ) {
        return;
    }
    // Deleted, the store's changed() reloads the tiles.
    if ( const auto problem = profiles_->remove( name ); !problem.isEmpty() ) {
        QMessageBox::warning( this, QStringLiteral( "Serial Monitor" ), problem );
        reload(); // e.g. deleted meanwhile
    }
}

void SidebarWidget::showRunning( ConnectionTile* tile, const SerialProcess& capture )
{
    const auto name = tile->name();
    auto sample = samples_.find( name );
    if ( sample == samples_.end() ) {
        // Started: the rate counts from here.
        sample = samples_.insert( name, { capture.totalLineCount(), 0, {} } );
        sample->since.start();
    }
    tile->setState( TileState::Running );
    tile->setDetails( detailsOf( capture, sample->rate ) );
}

void SidebarWidget::updateTiles()
{
    // The ports saved now, for the stopped tiles whose port another
    // configuration captures.
    QHash<QString, QString> savedPorts;
    for ( const auto& profile : profiles_->list() ) {
        savedPorts.insert( profile.name, profile.serial.portName );
    }
    for ( auto* tile : std::as_const( tileByName_ ) ) {
        const auto name = tile->name();
        if ( const auto* capture = sessions_->captureOf( name ) ) {
            showRunning( tile, *capture );
            continue;
        }
        samples_.remove( name );
        tile->setState( TileState::Stopped );
        const auto inUse = sessions_->portInUse( savedPorts.value( name ) );
        tile->setDetails( inUse.isEmpty() ? QStringList() : QStringList{ inUse } );
    }
    const bool anyRunning = sessions_->activeSessionCount() > 0;
    stopAll_->setEnabled( anyRunning );
    // The counters are read while some capture runs, not otherwise.
    if ( anyRunning && !timer_->isActive() ) {
        timer_->start();
    }
    else if ( !anyRunning ) {
        timer_->stop();
    }
}

} // namespace serial_monitor
