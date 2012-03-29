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

#ifndef __ZMQ_SESSION_HPP_INCLUDED__
#define __ZMQ_SESSION_HPP_INCLUDED__

#include "own.hpp"
#include "i_inout.hpp"
#include "io_object.hpp"
#include "blob.hpp"
#include "pipe.hpp"

namespace zmq
{

    class session_t :
        public own_t,
        public io_object_t,
        public i_inout,
        public i_reader_events,
        public i_writer_events
    {
    public:

        session_t (class io_thread_t *io_thread_,
            class socket_base_t *socket_, const options_t &options_);

        //  i_inout interface implementation. Note that detach method is not
        //  implemented by generic session. Different session types may handle
        //  engine disconnection in different ways.
        bool read (::zmq_msg_t *msg_);
        bool write (::zmq_msg_t *msg_);
        void flush ();
        void detach ();

        void attach_pipes (class reader_t *inpipe_, class writer_t *outpipe_,
            const blob_t &peer_identity_);

        //  i_reader_events interface implementation.
        void activated (class reader_t *pipe_);
        void terminated (class reader_t *pipe_);
        void delimited (class reader_t *pipe_);

        //  i_writer_events interface implementation.
        void activated (class writer_t *pipe_);
        void terminated (class writer_t *pipe_);

    protected:

        //  This function allows to shut down the session even though
        //  there are pending messages in the inbound pipe.
        void terminate ();

        //  Two events for the derived session type. Attached is triggered
        //  when session is attached to a peer, detached is triggered at the
        //  beginning of the termination process when session is about to
        //  be detached from the peer.
        virtual void attached (const blob_t &peer_identity_) = 0;
        virtual void detached () = 0;

        //  Allows derives session types to (un)register session names.
        bool register_session (const blob_t &name_, class session_t *session_);
        void unregister_session (const blob_t &name_);

        ~session_t ();

    private:

        //  Handlers for incoming commands.
        void process_plug ();
        void process_attach (struct i_engine *engine_,
            const blob_t &peer_identity_);
        void process_term (int linger_);

        //  i_poll_events handlers.
        void timer_event (int id_);

        //  Remove any half processed messages. Flush unflushed messages.
        //  Call this function when engine disconnect to get rid of leftovers.
        void clean_pipes ();

        //  Call this function to move on with the delayed process_term.
        void proceed_with_term ();

        //  Inbound pipe, i.e. one the session is getting messages from.
        class reader_t *in_pipe;

        //  This flag is true if the remainder of the message being processed
        //  is still in the in pipe.
        bool incomplete_in;

        //  Outbound pipe, i.e. one the socket is sending messages to.
        class writer_t *out_pipe;

        //  The protocol I/O engine connected to the session.
        struct i_engine *engine;

        //  The socket the session belongs to.
        class socket_base_t *socket;

        //  I/O thread the session is living in. It will be used to plug in
        //  the engines into the same thread.
        class io_thread_t *io_thread;

        //  If true, pipes were already attached to this session.
        bool pipes_attached;

        //  If true, delimiter was already read from the inbound pipe.
        bool delimiter_processed;

        //  If true, we should terminate the session even though there are
        //  pending messages in the inbound pipe.
        bool force_terminate;

        //  ID of the linger timer
        enum {linger_timer_id = 0x20};

        //  True is linger timer is running.
        bool has_linger_timer;

        enum {
            active,
            pending,
            terminating
        } state;

        session_t (const session_t&);
        const session_t &operator = (const session_t&);
    };

}

#endif
