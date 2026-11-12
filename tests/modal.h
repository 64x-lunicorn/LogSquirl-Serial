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
 * @file modal.h
 * @brief Answer the modal message boxes a click opens, for tests.
 *
 * A modal box runs its own event loop until it is answered, so a test
 * answers it from a timer started before the click (as LogSquirl-NATS's
 * plugin_test.cpp does).
 */

#pragma once

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include <memory>

namespace serial_test {

/// Answer the next question with @p button, and a warning that follows it
/// with OK; the warning's text, once it was shown, else empty.
inline std::shared_ptr<QString> answerQuestion( QMessageBox::StandardButton button )
{
    auto warning = std::make_shared<QString>();
    auto* timer = new QTimer;
    timer->setInterval( 10 );
    auto ticks = std::make_shared<int>( 0 );
    auto answered = std::make_shared<bool>( false );
    QObject::connect( timer, &QTimer::timeout, [ timer, warning, ticks, answered, button ] {
        // The question, then for 200 ms a warning that may follow it.
        if ( ++*ticks > ( *answered ? 20 : 500 ) ) {
            timer->stop();
            timer->deleteLater();
            return;
        }
        auto* box = qobject_cast<QMessageBox*>( QApplication::activeModalWidget() );
        if ( !box || !box->isVisible() ) {
            return;
        }
        if ( box->icon() == QMessageBox::Question && !*answered ) {
            *answered = true;
            *ticks = 0;
            box->button( button )->click();
        }
        else if ( box->icon() == QMessageBox::Warning ) {
            *warning = box->text();
            box->button( QMessageBox::Ok )->click();
            timer->stop();
            timer->deleteLater();
        }
    } );
    timer->start();
    return warning;
}

} // namespace serial_test
