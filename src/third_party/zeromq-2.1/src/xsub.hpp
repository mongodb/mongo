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

#ifndef __ZMQ_XSUB_HPP_INCLUDED__
#define __ZMQ_XSUB_HPP_INCLUDED__

#include "../include/zmq.h"

#include "trie.hpp"
#include "socket_base.hpp"
#include "fq.hpp"

namespace zmq
{

    class xsub_t : public socket_base_t
    {
    public:

        xsub_t (class ctx_t *parent_, uint32_t tid_);
        ~xsub_t ();

    protected:

        //  Overloads of functions from socket_base_t.
        void xattach_pipes (class reader_t *inpipe_, class writer_t *outpipe_,
            const blob_t &peer_identity_);
        int xsend (zmq_msg_t *msg_, int options_);
        bool xhas_out ();
        int xrecv (zmq_msg_t *msg_, int flags_);
        bool xhas_in ();

    private:

        //  Hook into the termination process.
        void process_term (int linger_);

        //  Check whether the message matches at least one subscription.
        bool match (zmq_msg_t *msg_);

        //  Fair queueing object for inbound pipes.
        fq_t fq;

        //  The repository of subscriptions.
        trie_t subscriptions;

        //  If true, 'message' contains a matching message to return on the
        //  next recv call.
        bool has_message;
        zmq_msg_t message;

        //  If true, part of a multipart message was already received, but
        //  there are following parts still waiting.
        bool more;

        xsub_t (const xsub_t&);
        const xsub_t &operator = (const xsub_t&);
    };

}

#endif

