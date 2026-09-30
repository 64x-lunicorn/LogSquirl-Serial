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
 * @file baudrate.h
 * @brief The baud rate combo box shared by the dialog and the sidebar.
 *
 * The combo offers the common rates but is editable, so any positive
 * rate the device needs can be typed in or come from the saved default
 * (Plugins → Configure accepts any rate).
 */

#pragma once

class QComboBox;

namespace serial_monitor {

/** Fill @p combo with the standard rates, make it editable and select 115200. */
void initBaudRateCombo( QComboBox* combo );

/** Select @p rate in @p combo, inserting it in order if it is not listed yet. */
void selectBaudRate( QComboBox* combo, int rate );

/** The rate shown in @p combo, or 0 if its text is not a positive number. */
int baudRateFrom( const QComboBox* combo );

} // namespace serial_monitor
