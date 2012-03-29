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

#ifndef __ZMQ_KQUEUE_HPP_INCLUDED__
#define __ZMQ_KQUEUE_HPP_INCLUDED__

#include "platform.hpp"

#if defined ZMQ_HAVE_FREEBSD || defined ZMQ_HAVE_OPENBSD ||\
    defined ZMQ_HAVE_OSX || defined ZMQ_HAVE_NETBSD

#include <vector>

#include "fd.hpp"
#include "thread.hpp"
#include "poller_base.hpp"

namespace zmq
{

    //  Implements socket polling mechanism using the BSD-specific
    //  kqueue interface.

    class kqueue_t : public poller_base_t
    {
    public:

        typedef void* handle_t;

        kqueue_t ();
        ~kqueue_t ();

        //  "poller" concept.
        handle_t add_fd (fd_t fd_, struct i_poll_events *events_);
        void rm_fd (handle_t handle_);
        void set_pollin (handle_t handle_);
        void reset_pollin (handle_t handle_);
        void set_pollout (handle_t handle_);
        void reset_pollout (handle_t handle_);
        void start ();
        void stop ();

    private:

        //  Main worker thread routine.
        static void worker_routine (void *arg_);

        //  Main event loop.
        void loop ();

        //  File descriptor referring to the kernel event queue.
        fd_t kqueue_fd;

        //  Adds the event to the kqueue.
        void kevent_add (fd_t fd_, short filter_, void *udata_);

        //  Deletes the event from the kqueue.
        void kevent_delete (fd_t fd_, short filter_);

        struct poll_entry_t
        {
            fd_t fd;
            bool flag_pollin;
            bool flag_pollout;
            i_poll_events *reactor;
        };

        //  List of retired event sources.
        typedef std::vector <poll_entry_t*> retired_t;
        retired_t retired;

        //  If true, thread is in the process of shutting down.
        bool stopping;

        //  Handle of the physical thread doing the I/O work.
        thread_t worker;

        kqueue_t (const kqueue_t&);
        const kqueue_t &operator = (const kqueue_t&);
    };

}

#endif

#endif
