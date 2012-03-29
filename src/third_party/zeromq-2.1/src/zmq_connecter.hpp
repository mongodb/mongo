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

#ifndef __ZMQ_ZMQ_CONNECTER_HPP_INCLUDED__
#define __ZMQ_ZMQ_CONNECTER_HPP_INCLUDED__

#include "own.hpp"
#include "io_object.hpp"
#include "tcp_connecter.hpp"
#include "stdint.hpp"

namespace zmq
{

    class zmq_connecter_t : public own_t, public io_object_t
    {
    public:

        //  If 'wait' is true connecter first waits for a while, then starts
        //  connection process.
        zmq_connecter_t (class io_thread_t *io_thread_,
            class session_t *session_, const options_t &options_,
            const char *protocol_, const char *address_, bool delay_);
        ~zmq_connecter_t ();

    private:

        //  ID of the timer used to delay the reconnection.
        enum {reconnect_timer_id = 1};

        //  Handlers for incoming commands.
        void process_plug ();

        //  Handlers for I/O events.
        void in_event ();
        void out_event ();
        void timer_event (int id_);

        //  Internal function to start the actual connection establishment.
        void start_connecting ();

        //  Internal function to add a reconnect timer
        void add_reconnect_timer();

        //  Internal function to return a reconnect backoff delay.
        //  Will modify the current_reconnect_ivl used for next call
        //  Returns the currently used interval
        int get_new_reconnect_ivl ();

        //  Actual connecting socket.
        tcp_connecter_t tcp_connecter;

        //  Handle corresponding to the listening socket.
        handle_t handle;

        //  If true file descriptor is registered with the poller and 'handle'
        //  contains valid value.
        bool handle_valid;

        //  If true, connecter is waiting a while before trying to connect.
        bool wait;

        //  Reference to the session we belong to.
        class session_t *session;

        //  Current reconnect ivl, updated for backoff strategy
        int current_reconnect_ivl;

        zmq_connecter_t (const zmq_connecter_t&);
        const zmq_connecter_t &operator = (const zmq_connecter_t&);
    };

}

#endif
