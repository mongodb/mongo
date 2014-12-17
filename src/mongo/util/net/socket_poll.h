/**
 * Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#ifndef _WIN32
# include <sys/poll.h>
#else
# if defined(NTDDI_VERSION) && ( !defined(NTDDI_VISTA) || ( NTDDI_VERSION < NTDDI_VISTA ) )
    // These are only defined in winsock2.h on newer windows but we need them everywhere.
#   define POLLRDNORM  0x0100
#   define POLLRDBAND  0x0200
#   define POLLIN      (POLLRDNORM | POLLRDBAND)
#   define POLLPRI     0x0400

#   define POLLWRNORM  0x0010
#   define POLLOUT     (POLLWRNORM)
#   define POLLWRBAND  0x0020

#   define POLLERR     0x0001
#   define POLLHUP     0x0002
#   define POLLNVAL    0x0004

    struct pollfd {
        SOCKET fd;
        SHORT events;
        SHORT revents;
    };
# endif // old windows
#endif // ndef _WIN32

namespace mongo {
    bool isPollSupported();
    int socketPoll(pollfd* fdarray, unsigned long nfds, int timeout);
}
