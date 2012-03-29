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

#ifndef __ZMQ_REP_HPP_INCLUDED__
#define __ZMQ_REP_HPP_INCLUDED__

#include "xrep.hpp"

namespace zmq
{

    class rep_t : public xrep_t
    {
    public:

        rep_t (class ctx_t *parent_, uint32_t tid_);
        ~rep_t ();

        //  Overloads of functions from socket_base_t.
        int xsend (zmq_msg_t *msg_, int flags_);
        int xrecv (zmq_msg_t *msg_, int flags_);
        bool xhas_in ();
        bool xhas_out ();

    private:

        //  If true, we are in process of sending the reply. If false we are
        //  in process of receiving a request.
        bool sending_reply;

        //  If true, we are starting to receive a request. The beginning
        //  of the request is the backtrace stack.
        bool request_begins;

        rep_t (const rep_t&);
        const rep_t &operator = (const rep_t&);

    };

}

#endif
