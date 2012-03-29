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

#ifndef __ZMQ_OBJECT_HPP_INCLUDED__
#define __ZMQ_OBJECT_HPP_INCLUDED__

#include "../include/zmq.h"

#include "stdint.hpp"
#include "blob.hpp"

namespace zmq
{
    //  Base class for all objects that participate in inter-thread
    //  communication.

    class object_t
    {
    public:

        object_t (class ctx_t *ctx_, uint32_t tid_);
        object_t (object_t *parent_);
        virtual ~object_t ();

        uint32_t get_tid ();
        ctx_t *get_ctx ();
        void process_command (struct command_t &cmd_);

    protected:

        //  Using following function, socket is able to access global
        //  repository of inproc endpoints.
        int register_endpoint (const char *addr_, struct endpoint_t &endpoint_);
        void unregister_endpoints (class socket_base_t *socket_);
        struct endpoint_t find_endpoint (const char *addr_);
        void destroy_socket (class socket_base_t *socket_);

        //  Logs an message.
        void log (const char *format_, ...);

        //  Chooses least loaded I/O thread.
        class io_thread_t *choose_io_thread (uint64_t affinity_);

        //  Derived object can use these functions to send commands
        //  to other objects.
        void send_stop ();
        void send_plug (class own_t *destination_,
            bool inc_seqnum_ = true);
        void send_own (class own_t *destination_,
            class own_t *object_);
        void send_attach (class session_t *destination_,
             struct i_engine *engine_, const blob_t &peer_identity_,
             bool inc_seqnum_ = true);
        void send_bind (class own_t *destination_,
             class reader_t *in_pipe_, class writer_t *out_pipe_,
             const blob_t &peer_identity_, bool inc_seqnum_ = true);
        void send_activate_reader (class reader_t *destination_);
        void send_activate_writer (class writer_t *destination_,
             uint64_t msgs_read_);
        void send_pipe_term (class writer_t *destination_);
        void send_pipe_term_ack (class reader_t *destination_);
        void send_term_req (class own_t *destination_,
            class own_t *object_);
        void send_term (class own_t *destination_, int linger_);
        void send_term_ack (class own_t *destination_);
        void send_reap (class socket_base_t *socket_);
        void send_reaped ();
        void send_done ();

        //  These handlers can be overloaded by the derived objects. They are
        //  called when command arrives from another thread.
        virtual void process_stop ();
        virtual void process_plug ();
        virtual void process_own (class own_t *object_);
        virtual void process_attach (struct i_engine *engine_,
            const blob_t &peer_identity_);
        virtual void process_bind (class reader_t *in_pipe_,
            class writer_t *out_pipe_, const blob_t &peer_identity_);
        virtual void process_activate_reader ();
        virtual void process_activate_writer (uint64_t msgs_read_);
        virtual void process_pipe_term ();
        virtual void process_pipe_term_ack ();
        virtual void process_term_req (class own_t *object_);
        virtual void process_term (int linger_);
        virtual void process_term_ack ();
        virtual void process_reap (class socket_base_t *socket_);
        virtual void process_reaped ();

        //  Special handler called after a command that requires a seqnum
        //  was processed. The implementation should catch up with its counter
        //  of processed commands here.
        virtual void process_seqnum ();

    private:

        //  Context provides access to the global state.
        class ctx_t *ctx;

        //  Thread ID of the thread the object belongs to.
        uint32_t tid;

        void send_command (command_t &cmd_);

        object_t (const object_t&);
        const object_t &operator = (const object_t&);
    };

}

#endif
