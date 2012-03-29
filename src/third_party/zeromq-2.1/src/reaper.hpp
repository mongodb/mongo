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

#ifndef __ZMQ_REAPER_HPP_INCLUDED__
#define __ZMQ_REAPER_HPP_INCLUDED__

#include "object.hpp"
#include "mailbox.hpp"
#include "poller.hpp"
#include "i_poll_events.hpp"

namespace zmq
{

    class reaper_t : public object_t, public i_poll_events
    {
    public:

        reaper_t (class ctx_t *ctx_, uint32_t tid_);
        ~reaper_t ();

        mailbox_t *get_mailbox ();

        void start ();
        void stop ();

        //  i_poll_events implementation.
        void in_event ();
        void out_event ();
        void timer_event (int id_);

    private:

        //  Command handlers.
        void process_stop ();
        void process_reap (class socket_base_t *socket_);
        void process_reaped ();

        //  Reaper thread accesses incoming commands via this mailbox.
        mailbox_t mailbox;

        //  Handle associated with mailbox' file descriptor.
        poller_t::handle_t mailbox_handle;

        //  I/O multiplexing is performed using a poller object.
        poller_t *poller;

        //  Number of sockets being reaped at the moment.
        int sockets;

        //  If true, we were already asked to terminate.
        bool terminating;

        reaper_t (const reaper_t&);
        const reaper_t &operator = (const reaper_t&);
    };

}

#endif
