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

#ifndef __ZMQ_ZMQ_LISTENER_HPP_INCLUDED__
#define __ZMQ_ZMQ_LISTENER_HPP_INCLUDED__

#include "own.hpp"
#include "io_object.hpp"
#include "tcp_listener.hpp"
#include "stdint.hpp"

namespace zmq
{

    class zmq_listener_t : public own_t, public io_object_t
    {
    public:

        zmq_listener_t (class io_thread_t *io_thread_,
            class socket_base_t *socket_, const options_t &options_);
        ~zmq_listener_t ();

        //  Set address to listen on.
        int set_address (const char* protocol_, const char *addr_);

    private:

        //  Handlers for incoming commands.
        void process_plug ();
        void process_term (int linger_);

        //  Handlers for I/O events.
        void in_event ();

        //  Actual listening socket.
        tcp_listener_t tcp_listener;

        //  Handle corresponding to the listening socket.
        handle_t handle;

        //  Socket the listerner belongs to.
        class socket_base_t *socket;

        zmq_listener_t (const zmq_listener_t&);
        const zmq_listener_t &operator = (const zmq_listener_t&);
    };

}

#endif
