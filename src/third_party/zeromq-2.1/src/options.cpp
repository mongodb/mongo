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

#include <string.h>
#ifndef ZMQ_HAVE_WINDOWS
#include <sys/stat.h>
#endif

#include "../include/zmq.h"
#include "options.hpp"
#include "err.hpp"

zmq::options_t::options_t () :
    hwm (0),
    swap (0),
    affinity (0),
    rate (40 * 1000),
    recovery_ivl (10),
    recovery_ivl_msec (-1),
    use_multicast_loop (true),
    sndbuf (0),
    rcvbuf (0),
    type (-1),
    linger (-1),
    reconnect_ivl (100),
    reconnect_ivl_max (0),
    backlog (100),
    requires_in (false),
    requires_out (false),
    immediate_connect (true)
{
}

int zmq::options_t::setsockopt (int option_, const void *optval_,
    size_t optvallen_)
{
    switch (option_) {

    case ZMQ_HWM:
        if (optvallen_ != sizeof (uint64_t)) {
            errno = EINVAL;
            return -1;
        }
        hwm = *((uint64_t*) optval_);
        return 0;

    case ZMQ_SWAP:
        if (optvallen_ != sizeof (int64_t) || *((int64_t*) optval_) < 0) {
            errno = EINVAL;
            return -1;
        }
        //  Check that SWAP directory (.) is writable
        struct stat stat_buf;
        if (stat (".", &stat_buf) || ((stat_buf.st_mode & S_IWRITE) == 0)) {
            errno = EACCES;
            return -1;
        }
        swap = *((int64_t*) optval_);
        return 0;

    case ZMQ_AFFINITY:
        if (optvallen_ != sizeof (uint64_t)) {
            errno = EINVAL;
            return -1;
        }
        affinity = *((uint64_t*) optval_);
        return 0;

    case ZMQ_IDENTITY:

        //  Empty identity is invalid as well as identity longer than
        //  255 bytes. Identity starting with binary zero is invalid
        //  as these are used for auto-generated identities.
        if (optvallen_ < 1 || optvallen_ > 255 ||
              *((const unsigned char*) optval_) == 0) {
            errno = EINVAL;
            return -1;
        }
        identity.assign ((const unsigned char*) optval_, optvallen_);
        return 0;

    case ZMQ_RATE:
        if (optvallen_ != sizeof (int64_t) || *((int64_t*) optval_) < 0) {
            errno = EINVAL;
            return -1;
        }
        rate = (uint32_t) *((int64_t*) optval_);
        return 0;
        
    case ZMQ_RECOVERY_IVL:
        if (optvallen_ != sizeof (int64_t)  || *((int64_t*) optval_) < 0) {
            errno = EINVAL;
            return -1;
        }
        recovery_ivl = (uint32_t) *((int64_t*) optval_);
        return 0;

    case ZMQ_RECOVERY_IVL_MSEC:
        if (optvallen_ != sizeof (int64_t)  || *((int64_t*) optval_) < 0) {
            errno = EINVAL;
            return -1;
        }
        recovery_ivl_msec = (int32_t) *((int64_t*) optval_);
        return 0;

    case ZMQ_MCAST_LOOP:
        if (optvallen_ != sizeof (int64_t)) {
            errno = EINVAL;
            return -1;
        }
        if ((int64_t) *((int64_t*) optval_) == 0)
            use_multicast_loop = false;
        else if ((int64_t) *((int64_t*) optval_) == 1)
            use_multicast_loop = true;
        else {
            errno = EINVAL;
            return -1;
        }
        return 0;

    case ZMQ_SNDBUF:
        if (optvallen_ != sizeof (uint64_t)) {
            errno = EINVAL;
            return -1;
        }
        sndbuf = *((uint64_t*) optval_);
        return 0;

    case ZMQ_RCVBUF:
        if (optvallen_ != sizeof (uint64_t)) {
            errno = EINVAL;
            return -1;
        }
        rcvbuf = *((uint64_t*) optval_);
        return 0;

    case ZMQ_LINGER:
        if (optvallen_ != sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        linger = *((int*) optval_);
        return 0;

    case ZMQ_RECONNECT_IVL:
        if (optvallen_ != sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        if (*((int*) optval_) < 0) {
            errno = EINVAL;
            return -1;
        }
        reconnect_ivl = *((int*) optval_);
        return 0;

    case ZMQ_RECONNECT_IVL_MAX:
        if (optvallen_ != sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        if (*((int*) optval_) < 0) {
            errno = EINVAL;
            return -1;
        }
        reconnect_ivl_max = *((int*) optval_);
        return 0;

    case ZMQ_BACKLOG:
        if (optvallen_ != sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        backlog = *((int*) optval_);
        return 0;

    }

    errno = EINVAL;
    return -1;
}

int zmq::options_t::getsockopt (int option_, void *optval_, size_t *optvallen_)
{
    switch (option_) {

    case ZMQ_HWM:
        if (*optvallen_ < sizeof (uint64_t)) {
            errno = EINVAL;
            return -1;
        }
        *((uint64_t*) optval_) = hwm;
        *optvallen_ = sizeof (uint64_t);
        return 0;

    case ZMQ_SWAP:
        if (*optvallen_ < sizeof (int64_t)) {
            errno = EINVAL;
            return -1;
        }
        *((int64_t*) optval_) = swap;
        *optvallen_ = sizeof (int64_t);
        return 0;

    case ZMQ_AFFINITY:
        if (*optvallen_ < sizeof (uint64_t)) {
            errno = EINVAL;
            return -1;
        }
        *((uint64_t*) optval_) = affinity;
        *optvallen_ = sizeof (uint64_t);
        return 0;

    case ZMQ_IDENTITY:
        if (*optvallen_ < identity.size ()) {
            errno = EINVAL;
            return -1;
        }
        memcpy (optval_, identity.data (), identity.size ());
        *optvallen_ = identity.size ();
        return 0;


    case ZMQ_RATE:
        if (*optvallen_ < sizeof (int64_t)) {
            errno = EINVAL;
            return -1;
        }
        *((int64_t*) optval_) = rate;
        *optvallen_ = sizeof (int64_t);
        return 0;
        
    case ZMQ_RECOVERY_IVL:
        if (*optvallen_ < sizeof (int64_t)) {
            errno = EINVAL;
            return -1;
        }
        *((int64_t*) optval_) = recovery_ivl;
        *optvallen_ = sizeof (int64_t);
        return 0;

    case ZMQ_RECOVERY_IVL_MSEC:
        if (*optvallen_ < sizeof (int64_t)) {
            errno = EINVAL;
            return -1;
        }
        *((int64_t*) optval_) = recovery_ivl_msec;
        *optvallen_ = sizeof (int64_t);
        return 0;

    case ZMQ_MCAST_LOOP:
        if (*optvallen_ < sizeof (int64_t)) {
            errno = EINVAL;
            return -1;
        }
        *((int64_t*) optval_) = use_multicast_loop ? 1 : 0;
        *optvallen_ = sizeof (int64_t);
        return 0;

    case ZMQ_SNDBUF:
        if (*optvallen_ < sizeof (uint64_t)) {
            errno = EINVAL;
            return -1;
        }
        *((uint64_t*) optval_) = sndbuf;
        *optvallen_ = sizeof (uint64_t);
        return 0;

    case ZMQ_RCVBUF:
        if (*optvallen_ < sizeof (uint64_t)) {
            errno = EINVAL;
            return -1;
        }
        *((uint64_t*) optval_) = rcvbuf;
        *optvallen_ = sizeof (uint64_t);
        return 0;

    case ZMQ_TYPE:
        if (*optvallen_ < sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        *((int*) optval_) = type;
        *optvallen_ = sizeof (int);
        return 0;

    case ZMQ_LINGER:
        if (*optvallen_ < sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        *((int*) optval_) = linger;
        *optvallen_ = sizeof (int);
        return 0;

    case ZMQ_RECONNECT_IVL:
        if (*optvallen_ < sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        *((int*) optval_) = reconnect_ivl;
        *optvallen_ = sizeof (int);
        return 0;

    case ZMQ_RECONNECT_IVL_MAX:
        if (*optvallen_ < sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        *((int*) optval_) = reconnect_ivl_max;
        *optvallen_ = sizeof (int);
        return 0;

    case ZMQ_BACKLOG:
        if (*optvallen_ < sizeof (int)) {
            errno = EINVAL;
            return -1;
        }
        *((int*) optval_) = backlog;
        *optvallen_ = sizeof (int);
        return 0;

    }

    errno = EINVAL;
    return -1;
}
