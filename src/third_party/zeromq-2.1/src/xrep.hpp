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

#ifndef __ZMQ_XREP_HPP_INCLUDED__
#define __ZMQ_XREP_HPP_INCLUDED__

#include <map>
#include <vector>

#include "socket_base.hpp"
#include "blob.hpp"
#include "pipe.hpp"

namespace zmq
{

    //  TODO: This class uses O(n) scheduling. Rewrite it to use O(1) algorithm.
    class xrep_t :
        public socket_base_t,
        public i_reader_events,
        public i_writer_events
    {
    public:

        xrep_t (class ctx_t *parent_, uint32_t tid_);
        ~xrep_t ();

        //  Overloads of functions from socket_base_t.
        void xattach_pipes (reader_t *inpipe_, writer_t *outpipe_,
            const blob_t &peer_identity_);
        int xsend (zmq_msg_t *msg_, int flags_);
        int xrecv (zmq_msg_t *msg_, int flags_);
        int rollback ();
        bool xhas_in ();
        bool xhas_out ();

    private:

        //  Hook into the termination process.
        void process_term (int linger_);

        //  i_reader_events interface implementation.
        void activated (reader_t *pipe_);
        void terminated (reader_t *pipe_);
        void delimited (reader_t *pipe_);

        //  i_writer_events interface implementation.
        void activated (writer_t *pipe_);
        void terminated (writer_t *pipe_);

        struct inpipe_t
        {
            class reader_t *reader;
            blob_t identity;
            bool active;
        };

        //  Inbound pipes with the names of corresponging peers.
        typedef std::vector <inpipe_t> inpipes_t;
        inpipes_t inpipes;

        //  The pipe we are currently reading from.
        inpipes_t::size_type current_in;

        //  Have we prefetched a message.
        bool prefetched;

        //  Holds the prefetched message.
        zmq_msg_t prefetched_msg;

        //  If true, more incoming message parts are expected.
        bool more_in;

        struct outpipe_t
        {
            class writer_t *writer;
            bool active;
        };

        //  Outbound pipes indexed by the peer names.
        typedef std::map <blob_t, outpipe_t> outpipes_t;
        outpipes_t outpipes;

        //  The pipe we are currently writing to.
        class writer_t *current_out;

        //  If true, more outgoing message parts are expected.
        bool more_out;

        //  If true, termination process is already underway.
        bool terminating;

        xrep_t (const xrep_t&);
        const xrep_t &operator = (const xrep_t&);
    };

}

#endif
