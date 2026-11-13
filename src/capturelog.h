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
 * @file capturelog.h
 * @brief What a capture writes to its log file, from the bytes it received
 *        and the commands it sent.
 *
 * A CaptureLog takes everything that happens after bytes arrive:
 *
 *   - CR/LF framing: "\n", "\r\n" and a lone "\r" all end a line, a CRLF
 *     split across two reads included;
 *   - a line that never ends is forced out at a maximum length, before a
 *     UTF-8 character whose last bytes have not arrived yet;
 *   - an optional timestamp before each line, and control characters
 *     written as visible `\xNN` escapes;
 *   - a sent command as a `[TX]` line, after the part of a line received
 *     so far, so that the rest of that line is not joined to it;
 *   - the line and byte counters.
 *
 * It writes to a QIODevice it is handed and does not own (a QFileDevice is
 * flushed after every call, so that a followed tab shows each line at
 * once).  It reads no clock but the one it is given, so tests can fix the
 * timestamps, and needs neither a port nor an event loop.
 */

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QIODevice>

#include <functional>

namespace serial_monitor {

class CaptureLog {
public:
    /// What a line's timestamp is taken from.
    using Clock = std::function<QDateTime()>;

    /// Longest line received that is waited for before it is forced out.
    static constexpr qsizetype kMaxLineLength = 64 * 1024;

    /**
     * @param timestamps     Prepend `[yyyy-MM-dd HH:mm:ss.zzz] ` to each line.
     * @param clock          The time a line is written; empty: the system's.
     * @param maxLineLength  Size above which a line received is forced out.
     */
    explicit CaptureLog( bool timestamps = true, Clock clock = {},
                         qsizetype maxLineLength = kMaxLineLength );

    /**
     * Begin a capture into @p file, open for writing: every counter starts
     * again, the part of a line received before is dropped, and a failure
     * before is forgotten.  The file's size counts with what it holds
     * already (a file appended to).
     */
    void start( QIODevice* file );

    /**
     * Go on in @p file, a new log file of the same capture (a rotation):
     * lineCount() and fileSize() start again, at what @p file holds; the
     * other counters and the part of a line received so far carry on.
     */
    void startFile( QIODevice* file );

    /**
     * Take @p data as received from the device: write every line it
     * completes, and keep the rest for the next call.
     *
     * @return false if the file could not be written (hasFailed()).
     */
    bool receive( const QByteArray& data );

    /**
     * Write @p command, sent to the device, as a `[TX]` line.  The part of
     * a line received so far is written first, as a line of its own; a
     * UTF-8 character the device has only begun stays for the rest of its
     * line, kept whole.
     *
     * @return false if the file could not be written (hasFailed()).
     */
    bool send( const QByteArray& command );

    /**
     * Write the part of a line received so far as a line of its own, as
     * before the file is closed.  Nothing if there is none.
     *
     * @return false if it could not be written (hasFailed()); true if
     *         there was none.
     */
    bool endPartialLine();

    /// Whether part of a line has been received and not written yet.
    bool hasPartialLine() const
    {
        return !partial_.isEmpty();
    }

    /**
     * Whether a write or flush failed since start(): nothing is written any
     * more then, and every call returns false.
     */
    bool hasFailed() const
    {
        return failed_;
    }

    // ── Counters ─────────────────────────────────────────────────────

    /// Lines written to the current file; 0 again after startFile().
    qint64 lineCount() const
    {
        return lineCount_;
    }

    /// Lines written since start(), received ones and [TX] ones.
    qint64 totalLineCount() const
    {
        return totalLineCount_;
    }

    /// Bytes received since start(), as they came, before line splitting.
    qint64 receivedBytes() const
    {
        return receivedBytes_;
    }

    /**
     * Size of the current file: what it held at start() or startFile(),
     * plus what was written to it, timestamps and escapes included.
     */
    qint64 fileSize() const
    {
        return fileSize_;
    }

private:
    /// Write one line, timestamped if configured and with its controls
    /// escaped; false, and hasFailed(), if it could not be.
    bool writeLine( const QByteArray& line );

    /// Flush a QFileDevice; false, and hasFailed(), if it could not be.
    bool flush();

    bool timestamps_;
    Clock clock_;
    qsizetype maxLineLength_;

    QIODevice* file_ = nullptr;
    QByteArray partial_;        ///< Bytes received after the last line written.
    bool afterCr_ = false;      ///< Whether the data received so far ended with a CR.
    bool failed_ = false;       ///< Whether a write failed since start().
    qint64 lineCount_ = 0;      ///< Lines in the current file.
    qint64 totalLineCount_ = 0; ///< Lines since start().
    qint64 receivedBytes_ = 0;  ///< Bytes received since start().
    qint64 fileSize_ = 0;       ///< Size of the current file.
};

} // namespace serial_monitor
