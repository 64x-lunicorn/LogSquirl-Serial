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
 * @brief A capture's log file: which file it is, and what is written to it
 *        from the bytes received and the commands sent.
 *
 * A CaptureLog takes everything that happens after bytes arrive:
 *
 *   - the log file: a save path appended to, or a new file in a temporary
 *     directory of its own, opened only if it is a regular file;
 *   - rotation into a new file, by hand or at a size limit, and what is
 *     done when the new file cannot be created;
 *   - CR/LF framing: "\n", "\r\n" and a lone "\r" all end a line, a CRLF
 *     split across two reads included;
 *   - a line that never ends is forced out at a maximum length, before a
 *     UTF-8 character whose last bytes have not arrived yet;
 *   - an optional timestamp before each line, and control characters
 *     written as visible `\xNN` escapes;
 *   - a sent command as a `[TX]` line, after the part of a line received
 *     so far, so that the rest of that line is not joined to it;
 *   - the line, byte and rotation counters.
 *
 * open() captures into a file of its own; start() into a device it is
 * handed and does not own, such as a buffer, or a device that refuses
 * writes in a test.  A QFileDevice is flushed after every call, so that a
 * followed tab shows each line at once.  It reads no clock but the one it
 * is given, so tests can fix the timestamps, and needs neither a port nor
 * an event loop.
 */

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QTemporaryDir>

#include <functional>
#include <memory>

namespace serial_monitor {

class CaptureLog {
public:
    /// What a line's timestamp is taken from.
    using Clock = std::function<QDateTime()>;

    /// Longest line received that is waited for before it is forced out.
    static constexpr qsizetype kMaxLineLength = 64 * 1024;

    /// What rotate() does with the part of a line received so far.
    enum class PartialLine {
        WriteOut, ///< End it in the old file, as a line of its own.
        Keep      ///< Keep it for the new file, where the rest of it goes.
    };

    /**
     * @param timestamps     Prepend `[yyyy-MM-dd HH:mm:ss.zzz] ` to each line.
     * @param clock          The time a line is written; empty: the system's.
     * @param maxLineLength  Size above which a line received is forced out.
     */
    explicit CaptureLog( bool timestamps = true, Clock clock = {},
                         qsizetype maxLineLength = kMaxLineLength );

    /// Closes the log file; removes the temporary directory unless it was
    /// kept (keepTempDir()).
    ~CaptureLog();

    CaptureLog( const CaptureLog& ) = delete;
    CaptureLog& operator=( const CaptureLog& ) = delete;

    // ── File names ───────────────────────────────────────────────────

    /**
     * Return a path for a new log file in @p dir, named
     * `<yyyy-MM-dd_HHmmss>_<port>.log`.  Characters of the port name that
     * are not valid in file names (the '/' of "/dev/ttyUSB0") are
     * replaced by '_'.  If that file exists, a number is appended
     * (`…_2.log`, `…_3.log`, …), so that two captures within the same
     * second never share a file.
     *
     * @param dir        Directory the file will be created in.
     * @param portName   Serial port name.
     * @param timestamp  Time the capture starts.
     * @return Absolute path of a file that does not exist yet.
     */
    static QString generateLogPath( const QString& dir, const QString& portName,
                                    const QDateTime& timestamp = QDateTime::currentDateTime() );

    /**
     * The path a user typed, made absolute: a leading `~` is the home
     * directory, and the path is cleaned.  Empty for a path that is still
     * relative, as `open_file()` takes absolute paths only and LogSquirl's
     * working directory is nothing the user chose (`/` when started from
     * the Finder or the Dock).
     */
    static QString absoluteUserPath( const QString& path );

    // ── The log file ─────────────────────────────────────────────────

    /**
     * Begin a capture of @p portName into a log file of its own, as
     * start() does into a device.  Nothing is ever truncated:
     *
     *   - with a @p savePath, that file, appended to (so that a capture
     *     before in it is kept), its directory created if need be; a path
     *     that is not a full one (absoluteUserPath()) is refused before
     *     anything is created;
     *   - without, a new file `serial_<port>.log` in a temporary directory
     *     of this CaptureLog, created below @p tempRoot (tempdirs.h) the
     *     first time.
     *
     * Only a regular file is opened: a FIFO would block the UI thread, a
     * symlink lead elsewhere, a device take the capture.  A file it
     * creates is readable by the user only.
     *
     * @return false, with what to tell the user in @p error, if no file
     *         could be opened.
     */
    bool open( const QString& portName, const QString& savePath, const QString& tempRoot,
               QString& error );

    /**
     * Close the log file of a capture that did not start after all, and
     * remove it if open() created it: a file appended to is kept.
     */
    void discard();

    /**
     * End the capture: write the part of a line received so far, as a
     * line of its own, and close the file.  It stays on disk.
     *
     * @return false if that part could not be written (hasFailed()).
     */
    bool close();

    /**
     * Go on in a new log file, which must not exist yet: in the save
     * path's directory, named as generateLogPath() names one; in the
     * temporary directory, `serial_<port>_<n>.log`, n counting the
     * rotations.  The old file is closed and stays on disk.  lineCount()
     * and fileSize() start again, and the size limit is due at its size
     * again.
     *
     * If the new file cannot be created, the capture goes on in the old
     * one, reopened, and @p error says so; the size limit stays where it
     * was.  If the old file cannot be reopened either, @p error says so
     * too, and the capture has ended: isOpen() is false, and a part of a
     * line kept for the new file is dropped, so close() has nothing left
     * to fail on.
     *
     * Not for a capture start()ed into a device, which it leaves as it is.
     *
     * @param partialLine  Where the part of a line received so far goes.
     * @return The new file's path; empty if the capture did not move.
     */
    QString rotate( PartialLine partialLine, QString& error );

    /// Whether a log file of open() is open: not before it, after close(),
    /// nor after a rotate() that could not reopen the old file.
    bool isOpen() const
    {
        return file_.isOpen();
    }

    /// The log file open()ed or rotated to last; empty before, or after a
    /// failed open() or discard().
    const QString& path() const
    {
        return path_;
    }

    /// Whether open() was given a save path, rather than a temporary file.
    bool isUsingSavePath() const
    {
        return usingSavePath_;
    }

    /**
     * Keep the temporary directory, with the log files in it, when this
     * CaptureLog is destroyed, for the tabs that show them.
     *
     * @return The directory kept; empty when there is none (a save path).
     */
    QString keepTempDir();

    // ── The size limit ───────────────────────────────────────────────

    /// Called after the line that made the file reach the size limit.
    using SizeLimitReached = std::function<void()>;

    /**
     * Ask for a rotation at @p bytes: @p reached is called after the line
     * that makes the current file reach that size (a file appended to with
     * what it held already).  If the file is not rotated then (a rotation
     * that failed), it is called again once the file has grown by the
     * limit once more, past the end of that line, not on every line.
     * 0: no limit.
     *
     * @p reached is called while a line is being written: it must not
     * write to this CaptureLog nor rotate it, but have that done later.
     */
    void setSizeLimit( qint64 bytes, SizeLimitReached reached );

    /// Whether the current file has reached the size limit; false without one.
    bool hasReachedSizeLimit() const
    {
        return sizeLimit_ > 0 && fileSize_ >= sizeLimit_;
    }

    /**
     * File size at which the size limit is due next: the limit after a
     * start or a rotation, further on after one that was asked for and did
     * not happen.  0 without a limit, or before a start.
     */
    qint64 nextRotationSize() const
    {
        return sizeLimit_ > 0 ? nextRotationAt_ : 0;
    }

    // ── Writing ──────────────────────────────────────────────────────

    /**
     * Begin a capture into @p file, open for writing, which the caller
     * keeps: every counter starts again, the part of a line received
     * before is dropped, and a failure before is forgotten.  The file's
     * size counts with what it holds already (a file appended to).
     */
    void start( QIODevice* file );

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

    /**
     * Why the write that failed did: the file it went to and the system's
     * reason, `Could not write the log file <path> (<reason>)`.  Empty
     * unless hasFailed().
     */
    const QString& writeError() const
    {
        return writeError_;
    }

    // ── Counters ─────────────────────────────────────────────────────

    /// Lines written to the current file; 0 again after a rotation.
    qint64 lineCount() const
    {
        return lineCount_;
    }

    /// Lines written since the start, received ones and [TX] ones.
    qint64 totalLineCount() const
    {
        return totalLineCount_;
    }

    /// Bytes received since the start, as they came, before line splitting.
    qint64 receivedBytes() const
    {
        return receivedBytes_;
    }

    /**
     * Size of the current file: what it held at the start or the rotation,
     * plus what was written to it, timestamps and escapes included.
     */
    qint64 fileSize() const
    {
        return fileSize_;
    }

    /// Rotations into a new file since open() (rotate() that succeeded).
    int rotationCount() const
    {
        return rotationCount_;
    }

private:
    /// Go on in @p file, a new log file of the same capture: lineCount()
    /// and fileSize() start again, at what @p file holds, and so does the
    /// size limit; the other counters and the partial line carry on.
    void startFile( QIODevice* file );

    /// Open @p path into file_ with @p mode, refusing anything that is not
    /// a regular file; false, with the reason in @p error.
    bool openFile( const QString& path, QIODevice::OpenMode mode, QString& error );

    /// Write one line, timestamped if configured and with its controls
    /// escaped; false, and hasFailed(), if it could not be.
    bool writeLine( const QByteArray& line );

    /// Flush a QFileDevice; false, and hasFailed(), if it could not be.
    bool flush();

    /// Remember that writing failed, and why.
    void fail();

    bool timestamps_;
    Clock clock_;
    qsizetype maxLineLength_;

    // The log file of open().  The directory is declared first, so that
    // the file is closed before the directory is removed.
    std::unique_ptr<QTemporaryDir> tempDir_; ///< Created by the first open() without save path.
    QFile file_;
    QString path_;     ///< The file open in file_, which has no name of its own.
    QString portName_; ///< What the rotated files are named after.
    QString saveDir_;  ///< The save path's directory; empty for a temporary file.
    bool usingSavePath_ = false;
    bool createdFile_ = false; ///< Whether open() created the file it opened.
    int rotationCount_ = 0;

    qint64 sizeLimit_ = 0; ///< 0: none.
    SizeLimitReached sizeLimitReached_;
    qint64 nextRotationAt_ = 0; ///< File size at which sizeLimitReached_ is due.

    QIODevice* device_ = nullptr; ///< Where lines are written: &file_, or start()'s.
    QByteArray partial_;          ///< Bytes received after the last line written.
    bool afterCr_ = false;        ///< Whether the data received so far ended with a CR.
    bool failed_ = false;         ///< Whether a write failed since the start.
    QString writeError_;          ///< Why it did.
    qint64 lineCount_ = 0;        ///< Lines in the current file.
    qint64 totalLineCount_ = 0;   ///< Lines since the start.
    qint64 receivedBytes_ = 0;    ///< Bytes received since the start.
    qint64 fileSize_ = 0;         ///< Size of the current file.
};

} // namespace serial_monitor
