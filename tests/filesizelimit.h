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
 * @file filesizelimit.h
 * @brief Makes writes to files fail as on a full disk, for tests of failing writes.
 *
 * Unix only: Windows has no per-process file size limit.
 */

#pragma once

#include <QtGlobal>

#ifdef Q_OS_UNIX

#include <csignal>
#include <sys/resource.h>

namespace serial_test {

/**
 * Limits the size of the files this process writes to @p bytes for its
 * lifetime: every write past it fails with EFBIG, as one to a full disk
 * fails.  The signal such a write would send is ignored meanwhile.
 */
class FileSizeLimit {
public:
    explicit FileSizeLimit( rlim_t bytes )
        : oldHandler_( std::signal( SIGXFSZ, SIG_IGN ) )
    {
        if ( ::getrlimit( RLIMIT_FSIZE, &before_ ) == 0 ) {
            auto limited = before_;
            limited.rlim_cur = bytes;
            active_ = ::setrlimit( RLIMIT_FSIZE, &limited ) == 0;
        }
    }

    ~FileSizeLimit()
    {
        lift();
        std::signal( SIGXFSZ, oldHandler_ );
    }

    FileSizeLimit( const FileSizeLimit& ) = delete;
    FileSizeLimit& operator=( const FileSizeLimit& ) = delete;

    /** Whether the limit is in force. */
    bool isActive() const
    {
        return active_;
    }

    /** Lift the limit before the end of the scope. */
    void lift()
    {
        if ( active_ ) {
            ::setrlimit( RLIMIT_FSIZE, &before_ );
            active_ = false;
        }
    }

private:
    struct rlimit before_{};
    void ( *oldHandler_ )( int );
    bool active_ = false;
};

} // namespace serial_test

#endif
