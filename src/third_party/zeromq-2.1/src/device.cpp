/*
    Copyright (c) 2007-2011 iMatix Corporation
    Copyright (c) 2007-2011 Other contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stddef.h>

#include "platform.hpp"

//  On AIX, poll.h has to be included before zmq.h to get consistent
//  definition of pollfd structure (AIX uses 'reqevents' and 'retnevents'
//  instead of 'events' and 'revents' and defines macros to map from POSIX-y
//  names to AIX-specific names).
#if defined ZMQ_HAVE_LINUX || defined ZMQ_HAVE_FREEBSD ||\
    defined ZMQ_HAVE_OPENBSD || defined ZMQ_HAVE_SOLARIS ||\
    defined ZMQ_HAVE_OSX || defined ZMQ_HAVE_QNXNTO ||\
    defined ZMQ_HAVE_HPUX || defined ZMQ_HAVE_AIX ||\
    defined ZMQ_HAVE_NETBSD
#include <poll.h>
#endif

#include "../include/zmq.h"

#include "device.hpp"
#include "socket_base.hpp"
#include "likely.hpp"
#include "err.hpp"

int zmq::device (class socket_base_t *insocket_,
        class socket_base_t *outsocket_)
{
    zmq_msg_t msg;
    int rc = zmq_msg_init (&msg);

    if (rc != 0) {
        return -1;
    }

    int64_t more;
    size_t moresz;

    zmq_pollitem_t items [2];
    items [0].socket = insocket_;
    items [0].fd = 0;
    items [0].events = ZMQ_POLLIN;
    items [0].revents = 0;
    items [1].socket = outsocket_;
    items [1].fd = 0;
    items [1].events = ZMQ_POLLIN;
    items [1].revents = 0;

    while (true) {

        //  Wait while there are either requests or replies to process.
        rc = zmq_poll (&items [0], 2, -1);
        if (unlikely (rc < 0)) {
            return -1;
        }

        //  The algorithm below asumes ratio of request and replies processed
        //  under full load to be 1:1. Although processing requests replies
        //  first is tempting it is suspectible to DoS attacks (overloading
        //  the system with unsolicited replies).

        //  Process a request.
        if (items [0].revents & ZMQ_POLLIN) {
            while (true) {

                rc = insocket_->recv (&msg, 0);
                if (unlikely (rc < 0)) {
                    return -1;
                }

                moresz = sizeof (more);
                rc = insocket_->getsockopt (ZMQ_RCVMORE, &more, &moresz);
                if (unlikely (rc < 0)) {
                    return -1;
                }

                rc = outsocket_->send (&msg, more ? ZMQ_SNDMORE : 0);
                if (unlikely (rc < 0)) {
                    return -1;
                }

                if (!more)
                    break;
            }
        }

        //  Process a reply.
        if (items [1].revents & ZMQ_POLLIN) {
            while (true) {

                rc = outsocket_->recv (&msg, 0);
                if (unlikely (rc < 0)) {
                    return -1;
                }

                moresz = sizeof (more);
                rc = outsocket_->getsockopt (ZMQ_RCVMORE, &more, &moresz);
                if (unlikely (rc < 0)) {
                    return -1;
                }

                rc = insocket_->send (&msg, more ? ZMQ_SNDMORE : 0);
                if (unlikely (rc < 0)) {
                    return -1;
                }

                if (!more)
                    break;
            }
        }

    }

    return 0;
}

