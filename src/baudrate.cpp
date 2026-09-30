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

#include "baudrate.h"

#include <QComboBox>
#include <QIntValidator>
#include <QList>

#include <limits>

namespace serial_monitor {

void initBaudRateCombo( QComboBox* combo )
{
    combo->setObjectName( "baudRate" );
    const QList<int> baudRates
        = { 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
    for ( const auto rate : baudRates ) {
        combo->addItem( QString::number( rate ), rate );
    }

    // Any positive rate can be typed in.  Typed rates are not added to
    // the list, so it stays sorted and free of typos.
    combo->setEditable( true );
    combo->setInsertPolicy( QComboBox::NoInsert );
    combo->setValidator( new QIntValidator( 1, std::numeric_limits<int>::max(), combo ) );

    selectBaudRate( combo, 115200 );
}

void selectBaudRate( QComboBox* combo, int rate )
{
    auto index = combo->findData( rate );
    if ( index < 0 ) {
        index = 0;
        while ( index < combo->count() && combo->itemData( index ).toInt() < rate ) {
            ++index;
        }
        combo->insertItem( index, QString::number( rate ), rate );
    }
    combo->setCurrentIndex( index );
}

int baudRateFrom( const QComboBox* combo )
{
    bool ok = false;
    const auto rate = combo->currentText().trimmed().toInt( &ok );
    return ok && rate > 0 ? rate : 0;
}

} // namespace serial_monitor
