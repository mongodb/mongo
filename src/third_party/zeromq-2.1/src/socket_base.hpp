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

#ifndef __ZMQ_SOCKET_BASE_HPP_INCLUDED__
#define __ZMQ_SOCKET_BASE_HPP_INCLUDED__

#include <map>
#include <vector>

#include "../include/zmq.h"

#include "own.hpp"
#include "array.hpp"
#include "mutex.hpp"
#include "stdint.hpp"
#include "poller.hpp"
#include "atomic_counter.hpp"
#include "i_poll_events.hpp"
#include "mailbox.hpp"
#include "stdint.hpp"
#include "blob.hpp"
#include "own.hpp"

namespace zmq
{

    class socket_base_t :
        public own_t,
        public array_item_t,
        public i_poll_events
    {
        friend class reaper_t;

    public:

        //  Returns false if object is not a socket.
        bool check_tag ();

        //  Create a socket of a specified type.
        static socket_base_t *create (int type_, class ctx_t *parent_,
            uint32_t tid_);

        //  Returns the mailbox associated with this socket.
        mailbox_t *get_mailbox ();

        //  Interrupt blocking call if the socket is stuck in one.
        //  This function can be called from a different thread!
        void stop ();

        //  Interface for communication with the API layer.
        int setsockopt (int option_, const void *optval_, size_t optvallen_);
        int getsockopt (int option_, void *optval_, size_t *optvallen_);
        int bind (const char *addr_);
        int connect (const char *addr_);
        int send (zmq_msg_t *msg_, int flags_);
        int recv (zmq_msg_t *msg_, int flags_);
        int close ();

        //  These functions are used by the polling mechanism to determine
        //  which events are to be reported from this socket.
        bool has_in ();
        bool has_out ();

        //  Registry of named sessions.
        bool register_session (const blob_t &name_, class session_t *session_);
        void unregister_session (const blob_t &name_);
        class session_t *find_session (const blob_t &name_);

        //  i_reader_events interface implementation.
        void activated (class reader_t *pipe_);
        void terminated (class reader_t *pipe_);

        //  i_writer_events interface implementation.
        void activated (class writer_t *pipe_);
        void terminated (class writer_t *pipe_);

        //  Using this function reaper thread ask the socket to regiter with
        //  its poller.
        void start_reaping (poller_t *poller_);

        //  i_poll_events implementation. This interface is used when socket
        //  is handled by the poller in the reaper thread.
        void in_event ();
        void out_event ();
        void timer_event (int id_);

        //  To be called after processing commands or invoking any command
        //  handlers explicitly. If required, it will deallocate the socket.
        void check_destroy ();

    protected:

        socket_base_t (class ctx_t *parent_, uint32_t tid_);
        virtual ~socket_base_t ();

        //  Concrete algorithms for the x- methods are to be defined by
        //  individual socket types.
        virtual void xattach_pipes (class reader_t *inpipe_,
            class writer_t *outpipe_, const blob_t &peer_identity_) = 0;

        //  The default implementation assumes there are no specific socket
        //  options for the particular socket type. If not so, overload this
        //  method.
        virtual int xsetsockopt (int option_, const void *optval_,
            size_t optvallen_);

        //  The default implementation assumes that send is not supported.
        virtual bool xhas_out ();
        virtual int xsend (zmq_msg_t *msg_, int options_);

        //  The default implementation assumes that recv in not supported.
        virtual bool xhas_in ();
        virtual int xrecv (zmq_msg_t *msg_, int options_);

        //  We are declaring termination handler as protected so that
        //  individual socket types can hook into the termination process
        //  by overloading it.
        void process_term (int linger_);

        //  Delay actual destruction of the socket.
        void process_destroy ();

    private:

        //  Used to check whether the object is a socket.
        uint32_t tag;

        //  If true, associated context was already terminated.
        bool ctx_terminated;

        //  If true, object should have been already destroyed. However,
        //  destruction is delayed while we unwind the stack to the point
        //  where it doesn't intersect the object being destroyed.
        bool destroyed;

        //  Parse URI string.
        int parse_uri (const char *uri_, std::string &protocol_,
            std::string &address_);

        //  Check whether transport protocol, as specified in connect or
        //  bind, is available and compatible with the socket type.
        int check_protocol (const std::string &protocol_);

        //  If no identity set generate one and call xattach_pipes ().
        void attach_pipes (class reader_t *inpipe_, class writer_t *outpipe_,
            const blob_t &peer_identity_);

        //  Processes commands sent to this socket (if any). If timeout is -1,
        //  returns only after at least one command was processed.
        //  If throttle argument is true, commands are processed at most once
        //  in a predefined time period.
        int process_commands (int timeout_, bool throttle_);

        //  Handlers for incoming commands.
        void process_stop ();
        void process_bind (class reader_t *in_pipe_, class writer_t *out_pipe_,
            const blob_t &peer_identity_);
        void process_unplug ();

        //  Socket's mailbox object.
        mailbox_t mailbox;

        //  Reaper's poller and handle of this socket within it.
        poller_t *poller;
        poller_t::handle_t handle;

        //  Timestamp of when commands were processed the last time.
        uint64_t last_tsc;

        //  Number of messages received since last command processing.
        int ticks;

        //  If true there's a half-read message in the socket.
        bool rcvmore;

        //  Lists of existing sessions. This list is never referenced from
        //  within the socket, instead it is used by objects owned by
        //  the socket. As those objects can live in different threads,
        //  the access is synchronised by mutex.
        typedef std::map <blob_t, session_t*> sessions_t;
        sessions_t sessions;
        mutex_t sessions_sync;

        socket_base_t (const socket_base_t&);
        const socket_base_t &operator = (const socket_base_t&);
    };

}

#endif
