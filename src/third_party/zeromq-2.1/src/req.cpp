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

#include "../include/zmq.h"

#include "req.hpp"
#include "err.hpp"

zmq::req_t::req_t (class ctx_t *parent_, uint32_t tid_) :
    xreq_t (parent_, tid_),
    receiving_reply (false),
    message_begins (true)
{
    options.type = ZMQ_REQ;
}

zmq::req_t::~req_t ()
{
}

int zmq::req_t::xsend (zmq_msg_t *msg_, int flags_)
{
    //  If we've sent a request and we still haven't got the reply,
    //  we can't send another request.
    if (receiving_reply) {
        errno = EFSM;
        return -1;
    }

    //  First part of the request is empty message part (stack bottom).
    if (message_begins) {
        zmq_msg_t prefix;
        int rc = zmq_msg_init (&prefix);
        zmq_assert (rc == 0);
        prefix.flags |= ZMQ_MSG_MORE;
        rc = xreq_t::xsend (&prefix, flags_);
        if (rc != 0)
            return rc;
        message_begins = false;
    }

    bool more = msg_->flags & ZMQ_MSG_MORE;

    int rc = xreq_t::xsend (msg_, flags_);
    if (rc != 0)
        return rc;

    //  If the request was fully sent, flip the FSM into reply-receiving state.
    if (!more) {
        receiving_reply = true;
        message_begins = true;
    }

    return 0;
}

int zmq::req_t::xrecv (zmq_msg_t *msg_, int flags_)
{
    //  If request wasn't send, we can't wait for reply.
    if (!receiving_reply) {
        errno = EFSM;
        return -1;
    }

    //  First part of the reply should be empty message part (stack bottom).
    if (message_begins) {
        int rc = xreq_t::xrecv (msg_, flags_);
        if (rc != 0)
            return rc;

        // TODO: this should also close the connection with the peer
        if (!(msg_->flags & ZMQ_MSG_MORE) || zmq_msg_size (msg_) != 0) {
            errno = EAGAIN;
            return -1;
        }
        message_begins = false;
    }

    int rc = xreq_t::xrecv (msg_, flags_);
    if (rc != 0)
        return rc;

    //  If the reply is fully received, flip the FSM into request-sending state.
    if (!(msg_->flags & ZMQ_MSG_MORE)) {
        receiving_reply = false;
        message_begins = true;
    }

    return 0;
}

bool zmq::req_t::xhas_in ()
{
    if (!receiving_reply)
        return false;

    return xreq_t::xhas_in ();
}

bool zmq::req_t::xhas_out ()
{
    if (receiving_reply)
        return false;

    return xreq_t::xhas_out ();
}


