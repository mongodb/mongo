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

#ifndef __ZMQ_PULL_HPP_INCLUDED__
#define __ZMQ_PULL_HPP_INCLUDED__

#include "socket_base.hpp"
#include "fq.hpp"

namespace zmq
{

    class pull_t : public socket_base_t
    {
    public:

        pull_t (class ctx_t *parent_, uint32_t tid_);
        ~pull_t ();

    protected:

        //  Overloads of functions from socket_base_t.
        void xattach_pipes (class reader_t *inpipe_, class writer_t *outpipe_,
            const blob_t &peer_identity_);
        int xrecv (zmq_msg_t *msg_, int flags_);
        bool xhas_in ();

    private:

        //  Hook into the termination process.
        void process_term (int linger_);

        //  Fair queueing object for inbound pipes.
        fq_t fq;

        pull_t (const pull_t&);
        const pull_t &operator = (const pull_t&);

    };

}

#endif
