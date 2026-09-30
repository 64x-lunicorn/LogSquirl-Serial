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
 * @file baudrate_test.cpp
 * @brief BDD tests for the baud rate combo boxes of the dialog and sidebar.
 *
 * Plugins → Configure accepts any baud rate, and so do the combo boxes:
 * a rate missing from the standard list is added in order rather than
 * silently ignored.
 */

#include <catch2/catch.hpp>

#include "baudrate.h"
#include "fakehost.h"
#include "portwidget.h"
#include "sidebarwidget.h"

#include <QComboBox>
#include <QLocale>
#include <QSettings>

#include <algorithm>

using serial_monitor::baudRateFrom;
using serial_monitor::initBaudRateCombo;
using serial_monitor::PortWidget;
using serial_monitor::selectBaudRate;
using serial_monitor::SerialConfig;
using serial_monitor::SidebarWidget;
using serial_test::FakeHost;

namespace {

QList<int> ratesIn( const QComboBox& combo )
{
    QList<int> rates;
    for ( int i = 0; i < combo.count(); ++i ) {
        rates << combo.itemData( i ).toInt();
    }
    return rates;
}

bool isSorted( const QList<int>& rates )
{
    return std::is_sorted( rates.cbegin(), rates.cend() );
}

void saveDefaultBaud( const FakeHost& host, int rate )
{
    QSettings settings( host.configDir() + "/serial.ini", QSettings::IniFormat );
    settings.setValue( "serial/defaultBaud", rate );
}

} // namespace

SCENARIO( "a baud rate combo accepts rates outside the standard list", "[baudrate]" )
{
    GIVEN( "a freshly set up baud rate combo" )
    {
        QComboBox combo;
        initBaudRateCombo( &combo );

        THEN( "it offers the standard rates, selects 115200 and is editable" )
        {
            REQUIRE( ratesIn( combo ).contains( 9600 ) );
            REQUIRE( isSorted( ratesIn( combo ) ) );
            REQUIRE( baudRateFrom( &combo ) == 115200 );
            REQUIRE( combo.isEditable() );
        }

        WHEN( "selecting a standard rate" )
        {
            const auto count = combo.count();
            selectBaudRate( &combo, 9600 );

            THEN( "it is selected without adding an entry" )
            {
                REQUIRE( baudRateFrom( &combo ) == 9600 );
                REQUIRE( combo.count() == count );
            }
        }

        WHEN( "selecting a rate that is not in the list" )
        {
            const auto count = combo.count();
            selectBaudRate( &combo, 250000 );

            THEN( "it is added in order and selected" )
            {
                REQUIRE( baudRateFrom( &combo ) == 250000 );
                REQUIRE( combo.count() == count + 1 );
                REQUIRE( isSorted( ratesIn( combo ) ) );
            }

            AND_WHEN( "selecting it again" )
            {
                selectBaudRate( &combo, 250000 );

                THEN( "it is not added twice" )
                {
                    REQUIRE( combo.count() == count + 1 );
                }
            }
        }

        WHEN( "typing a custom rate" )
        {
            combo.setEditText( "74880" );

            THEN( "the typed rate is used" )
            {
                REQUIRE( baudRateFrom( &combo ) == 74880 );
            }
        }

        WHEN( "the text is not a positive number" )
        {
            THEN( "no rate is reported" )
            {
                combo.setEditText( "" );
                REQUIRE( baudRateFrom( &combo ) == 0 );
                combo.setEditText( "0" );
                REQUIRE( baudRateFrom( &combo ) == 0 );
            }

            THEN( "the validator refuses letters and signs" )
            {
                int pos = 0;
                QString text = "fast";
                REQUIRE( combo.validator()->validate( text, pos ) == QValidator::Invalid );
                text = "-9600";
                REQUIRE( combo.validator()->validate( text, pos ) == QValidator::Invalid );
            }
        }
    }
}

SCENARIO( "a baud rate combo reads numbers the same way in every locale", "[baudrate]" )
{
    GIVEN( "a baud rate combo set up while the locale is German" )
    {
        const auto previousLocale = QLocale();
        QLocale::setDefault( QLocale( QLocale::German, QLocale::Germany ) );
        QComboBox combo;
        initBaudRateCombo( &combo );
        QLocale::setDefault( previousLocale );

        WHEN( "typing a rate with a group separator" )
        {
            THEN( "the validator refuses it, since it would not be read as a rate" )
            {
                int pos = 0;
                QString text = "250.000";
                REQUIRE( combo.validator()->validate( text, pos ) == QValidator::Invalid );
                text = "250,000";
                REQUIRE( combo.validator()->validate( text, pos ) == QValidator::Invalid );
            }
        }

        WHEN( "typing a plain rate" )
        {
            THEN( "the validator accepts it and it is read as typed" )
            {
                int pos = 0;
                QString text = "250000";
                REQUIRE( combo.validator()->validate( text, pos ) == QValidator::Acceptable );
                combo.setEditText( text );
                REQUIRE( baudRateFrom( &combo ) == 250000 );
            }
        }
    }
}

SCENARIO( "a custom default baud rate is applied to the dialog and sidebar", "[baudrate]" )
{
    GIVEN( "a default baud rate that is not in the standard list" )
    {
        FakeHost host;
        saveDefaultBaud( host, 250000 );

        WHEN( "the dialog and sidebar are created" )
        {
            PortWidget portWidget;
            SidebarWidget sidebar( &portWidget );

            THEN( "both select the configured rate" )
            {
                auto* dialogCombo = portWidget.findChild<QComboBox*>( "baudRate" );
                auto* sidebarCombo = sidebar.findChild<QComboBox*>( "baudRate" );
                REQUIRE( dialogCombo );
                REQUIRE( sidebarCombo );
                REQUIRE( baudRateFrom( dialogCombo ) == 250000 );
                REQUIRE( baudRateFrom( sidebarCombo ) == 250000 );
                REQUIRE( isSorted( ratesIn( *dialogCombo ) ) );
                REQUIRE( isSorted( ratesIn( *sidebarCombo ) ) );
            }
        }
    }
}

SCENARIO( "a session with an invalid baud rate is refused", "[baudrate]" )
{
    GIVEN( "a configuration without a usable baud rate" )
    {
        FakeHost host;
        PortWidget widget;
        SerialConfig cfg;
        cfg.portName = "logsquirl-test-port";
        cfg.baudRate = 0;

        WHEN( "starting a session" )
        {
            const auto started = widget.startSession( cfg );

            THEN( "it is rejected with a notification" )
            {
                REQUIRE_FALSE( started );
                REQUIRE( widget.activeSessionCount() == 0 );
                REQUIRE( host.notifications.size() == 1 );
                REQUIRE( host.notifications.first().contains( "baud rate" ) );
            }
        }
    }
}
