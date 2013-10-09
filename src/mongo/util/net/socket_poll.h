/**
 * Copyright 2012 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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
