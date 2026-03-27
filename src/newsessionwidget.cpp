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
 * @file newsessionwidget.cpp
 * @brief Implementation of the toolbar "New Session" button for serial.
 *
 * The button is registered as a status widget in the "Plugins" toolbar.
 * Its dropdown menu is rebuilt each time it is about to be shown, querying
 * the PortWidget for currently active sessions.
 */

#include "newsessionwidget.h"
#include "plugin.h"
#include "portwidget.h"

#include <QHBoxLayout>
#include <QStyle>

namespace serial_monitor {

NewSessionWidget::NewSessionWidget( QWidget* parent )
    : QWidget( parent )
{
    auto* layout = new QHBoxLayout( this );
    layout->setContentsMargins( 0, 0, 0, 0 );

    button_ = new QToolButton( this );
    button_->setToolTip( "New Session — rotate the log for an active serial port" );

    // Use a theme icon with a built-in fallback
    auto icon = QIcon::fromTheme( "document-new" );
    if ( icon.isNull() ) {
        icon = style()->standardIcon( QStyle::SP_FileDialogNewFolder );
    }
    button_->setIcon( icon );
    button_->setToolButtonStyle( Qt::ToolButtonIconOnly );

    menu_ = new QMenu( this );
    button_->setMenu( menu_ );
    button_->setPopupMode( QToolButton::InstantPopup );

    layout->addWidget( button_ );

    // Rebuild the menu every time it is about to be shown
    connect( menu_, &QMenu::aboutToShow, this, &NewSessionWidget::updateMenu );

    // Start disabled — enabled when sessions become active
    button_->setEnabled( false );
}

void NewSessionWidget::updateMenu()
{
    menu_->clear();

    if ( !g_state.dialog ) {
        button_->setEnabled( false );
        return;
    }

    const auto ports = g_state.dialog->activePorts();
    if ( ports.isEmpty() ) {
        button_->setEnabled( false );
        return;
    }

    button_->setEnabled( true );

    for ( const auto& portName : ports ) {
        auto* action = menu_->addAction( portName );
        connect( action, &QAction::triggered, this, [portName]() {
            if ( g_state.dialog ) {
                g_state.dialog->rotateSession( portName );
            }
        } );
    }
}

} // namespace serial_monitor
