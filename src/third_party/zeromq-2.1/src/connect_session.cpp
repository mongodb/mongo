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

#include "connect_session.hpp"
#include "zmq_connecter.hpp"
#include "pgm_sender.hpp"
#include "pgm_receiver.hpp"

zmq::connect_session_t::connect_session_t (class io_thread_t *io_thread_,
      class socket_base_t *socket_, const options_t &options_,
      const char *protocol_, const char *address_) :
    session_t (io_thread_, socket_, options_),
    protocol (protocol_),
    address (address_)
{
}

zmq::connect_session_t::~connect_session_t ()
{
}

void zmq::connect_session_t::process_plug ()
{
    //  Start connection process immediately.
    start_connecting (false);
}

void zmq::connect_session_t::start_connecting (bool wait_)
{
    //  Choose I/O thread to run connecter in. Given that we are already
    //  running in an I/O thread, there must be at least one available.
    io_thread_t *io_thread = choose_io_thread (options.affinity);
    zmq_assert (io_thread);

    //  Create the connecter object.

    //  Both TCP and IPC transports are using the same infrastructure.
    if (protocol == "tcp" || protocol == "ipc") {

        zmq_connecter_t *connecter = new (std::nothrow) zmq_connecter_t (
            io_thread, this, options, protocol.c_str (), address.c_str (),
            wait_);
        alloc_assert (connecter);
        launch_child (connecter);
        return;
    }

#if defined ZMQ_HAVE_OPENPGM

    //  Both PGM and EPGM transports are using the same infrastructure.
    if (protocol == "pgm" || protocol == "epgm") {

        //  For EPGM transport with UDP encapsulation of PGM is used.
        bool udp_encapsulation = (protocol == "epgm");

        //  At this point we'll create message pipes to the session straight
        //  away. There's no point in delaying it as no concept of 'connect'
        //  exists with PGM anyway.
        if (options.type == ZMQ_PUB || options.type == ZMQ_XPUB) {

            //  PGM sender.
            pgm_sender_t *pgm_sender =  new (std::nothrow) pgm_sender_t (
                io_thread, options);
            alloc_assert (pgm_sender);

            int rc = pgm_sender->init (udp_encapsulation, address.c_str ());
            zmq_assert (rc == 0);

            send_attach (this, pgm_sender, blob_t ());
        }
        else if (options.type == ZMQ_SUB || options.type == ZMQ_XSUB) {

            //  PGM receiver.
            pgm_receiver_t *pgm_receiver =  new (std::nothrow) pgm_receiver_t (
                io_thread, options);
            alloc_assert (pgm_receiver);

            int rc = pgm_receiver->init (udp_encapsulation, address.c_str ());
            zmq_assert (rc == 0);

            send_attach (this, pgm_receiver, blob_t ());
        }
        else
            zmq_assert (false);

        return;
    }
#endif

    zmq_assert (false);
}

void zmq::connect_session_t::attached (const blob_t &peer_identity_)
{
}

void zmq::connect_session_t::detached ()
{
    //  Reconnect.
    start_connecting (true);
}

