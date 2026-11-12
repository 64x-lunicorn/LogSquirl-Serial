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
#include "plugin.h"
#include "profiles.h"
#include "sessions.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <utility>

namespace serial_monitor {

namespace {

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

} // namespace

SidebarWidget::SidebarWidget( ProfileStore* profiles, Sessions* sessions, QWidget* parent )
    : QWidget( parent )
    , profiles_( profiles )
    , sessions_( sessions )
    , stopAll_( new QPushButton( QStringLiteral( "■ Stop all" ), this ) )
    , empty_( new QLabel(
          QStringLiteral( "No saved configuration yet: New configuration… adds one." ), this ) )
    , tiles_( new QVBoxLayout )
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
        guarded( "stopping every serial capture", [ this ] {
            sessions_->stopAll();
            hostNotify( QStringLiteral( "All serial sessions stopped." ) );
        } );
    } );
    const auto follow
        = [ this ] { guarded( "showing the serial captures", [ this ] { updateTiles(); } ); };
    connect( sessions_, &Sessions::sessionStarted, this, follow );
    connect( sessions_, &Sessions::sessionStopped, this, follow );
    connect( profiles_, &ProfileStore::changed, this,
             [ this ] { guarded( "showing the serial configurations", [ this ] { reload(); } ); } );

    reload();
}

ConnectionTile* SidebarWidget::tile( const QString& name ) const
{
    return tileByName_.value( name );
}

void SidebarWidget::reload()
{
    // Later: a tile's own click may have led here.
    for ( auto* tile : std::as_const( tileByName_ ) ) {
        tile->setParent( nullptr );
        tile->deleteLater();
    }
    tileByName_.clear();
    savedPortByName_.clear();

    const auto profiles = profiles_->list();
    for ( const auto& profile : profiles ) {
        auto* tile = new ConnectionTile( profile.name, this );
        tile->setSummary( summaryOf( profile ) );
        const auto name = profile.name;
        connect( tile, &ConnectionTile::startClicked, this, [ this, name ] {
            guarded( "starting a serial capture", [ this, &name ] { start( name ); } );
        } );
        connect( tile, &ConnectionTile::stopClicked, this, [ this, name ] {
            guarded( "stopping a serial capture", [ this, &name ] { stop( name ); } );
        } );
        connect( tile, &ConnectionTile::editClicked, this,
                 [ this, name ] { Q_EMIT editRequested( name ); } );
        connect( tile, &ConnectionTile::deleteClicked, this, [ this, name ] {
            guarded( "deleting a serial configuration",
                     [ this, &name ] { confirmDelete( name ); } );
        } );
        tiles_->addWidget( tile );
        tileByName_.insert( name, tile );
        savedPortByName_.insert( name, profile.serial.portName );
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
    if ( const auto problem = startProblem( *profile ); !problem.isEmpty() ) {
        const auto message
            = QStringLiteral( "Serial capture %1 not started: %2" ).arg( name, problem );
        hostLog( LOGSQUIRL_LOG_WARNING, message );
        hostNotify( message );
        return;
    }
    // Refused with a notification when the port is captured already, or
    // does not open; sessionStarted() updates the tile when it starts.
    sessions_->startSession( sessionConfig( *profile ), savePathFor( *profile ), name );
}

void SidebarWidget::stop( const QString& name )
{
    if ( const auto port = sessions_->portOf( name ); !port.isEmpty() ) {
        sessions_->stopSession( port );
    }
    else {
        updateTiles(); // it ended meanwhile
    }
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

void SidebarWidget::updateTile( ConnectionTile* tile )
{
    const auto name = tile->name();
    if ( !sessions_->portOf( name ).isEmpty() ) {
        tile->setState( TileState::Running );
        tile->setDetails( { QStringLiteral( "running" ) } );
        return;
    }
    tile->setState( TileState::Stopped );
    const auto port = savedPortByName_.value( name );
    if ( !port.isEmpty() && sessions_->isSessionActive( port ) ) {
        const auto owner = sessions_->ownerOf( port );
        tile->setDetails( { owner.isEmpty()
                                ? QStringLiteral( "%1 is in use by another capture" ).arg( port )
                                : QStringLiteral( "%1 is in use by “%2”" ).arg( port, owner ) } );
        return;
    }
    tile->setDetails( {} );
}

void SidebarWidget::updateTiles()
{
    for ( auto* tile : std::as_const( tileByName_ ) ) {
        updateTile( tile );
    }
    stopAll_->setEnabled( sessions_->activeSessionCount() > 0 );
}

} // namespace serial_monitor
