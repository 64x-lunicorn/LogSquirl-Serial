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
 * @file porterror.h
 * @brief Makes the serial port of a running capture report an error, for tests.
 *
 * A pseudo-terminal cannot be unplugged: QSerialPort does not notice when
 * its other side closes.  A real USB adapter that is pulled makes the port
 * emit errorOccurred(QSerialPort::ResourceError), and some drivers report
 * it, or a failure of their own, as a ReadError or an UnknownError.  For a
 * capture that a session manager created on a pseudo-terminal, the test
 * has its port emit that signal, as the port would; the capture handles it
 * as it handles any error of its port.
 */

#pragma once

#include <catch2/catch.hpp>

#include <QObject>
#include <QSerialPort>

namespace serial_test {

/** Have the serial port of the capture @p session report @p error. */
inline void reportPortError( QObject* session, QSerialPort::SerialPortError error )
{
    REQUIRE( session );
    auto* port = session->findChild<QSerialPort*>();
    REQUIRE( port );
    Q_EMIT port->errorOccurred( error );
}

} // namespace serial_test
