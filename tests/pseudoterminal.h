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
 * @file pseudoterminal.h
 * @brief A pseudo-terminal that stands in for a serial device, for tests.
 *
 * QSerialPort opens the terminal side of the pair like any tty; the test
 * plays the device by writing to (or closing) the other side.  Unix only:
 * Windows has no equivalent a QSerialPort can open.
 */

#pragma once

#include <QtGlobal>

#ifdef Q_OS_UNIX

#include <QByteArray>
#include <QString>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

namespace serial_test {

class PseudoTerminal {
public:
    PseudoTerminal()
        : controller_( posix_openpt( O_RDWR | O_NOCTTY ) )
    {
        if ( controller_ >= 0 && grantpt( controller_ ) == 0 && unlockpt( controller_ ) == 0 ) {
            devicePath_ = QString::fromLocal8Bit( ptsname( controller_ ) );
        }
    }

    ~PseudoTerminal()
    {
        unplug();
    }

    PseudoTerminal( const PseudoTerminal& ) = delete;
    PseudoTerminal& operator=( const PseudoTerminal& ) = delete;

    /** Path of the terminal device, for SerialConfig::portName. */
    QString devicePath() const
    {
        return devicePath_;
    }

    /** Send @p data as the device, i.e. to whoever opened devicePath(). */
    bool send( const QByteArray& data )
    {
        return ::write( controller_, data.constData(), static_cast<size_t>( data.size() ) )
               == data.size();
    }

    /** Close the device side, as if the device had been unplugged. */
    void unplug()
    {
        if ( controller_ >= 0 ) {
            ::close( controller_ );
            controller_ = -1;
        }
    }

private:
    int controller_ = -1;
    QString devicePath_;
};

} // namespace serial_test

#endif
