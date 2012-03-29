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

#ifndef __ZMQ_POLLER_HPP_INCLUDED__
#define __ZMQ_POLLER_HPP_INCLUDED__

#include "epoll.hpp"
#include "poll.hpp"
#include "select.hpp"
#include "devpoll.hpp"
#include "kqueue.hpp"

namespace zmq
{

#if defined ZMQ_FORCE_SELECT
    typedef select_t poller_t;
#elif defined ZMQ_FORCE_POLL
    typedef poll_t poller_t;
#elif defined ZMQ_FORCE_EPOLL
    typedef epoll_t poller_t;
#elif defined ZMQ_FORCE_DEVPOLL
    typedef devpoll_t poller_t;
#elif defined ZMQ_FORCE_KQUEUE
    typedef kqueue_t poller_t;
#elif defined ZMQ_HAVE_LINUX
    typedef epoll_t poller_t;
#elif defined ZMQ_HAVE_WINDOWS
    typedef select_t poller_t;
#elif defined ZMQ_HAVE_FREEBSD
    typedef kqueue_t poller_t;
#elif defined ZMQ_HAVE_OPENBSD
    typedef kqueue_t poller_t;
#elif defined ZMQ_HAVE_NETBSD
    typedef kqueue_t poller_t;
#elif defined ZMQ_HAVE_SOLARIS
    typedef devpoll_t poller_t;
#elif defined ZMQ_HAVE_OSX
    typedef kqueue_t poller_t;
#elif defined ZMQ_HAVE_QNXNTO
    typedef poll_t poller_t;
#elif defined ZMQ_HAVE_AIX
    typedef poll_t poller_t;
#elif defined ZMQ_HAVE_HPUX
    typedef devpoll_t poller_t;
#elif defined ZMQ_HAVE_OPENVMS
    typedef select_t poller_t;
#elif defined ZMQ_HAVE_CYGWIN
    typedef select_t poller_t;
#else
#error Unsupported platform
#endif

}

#endif
