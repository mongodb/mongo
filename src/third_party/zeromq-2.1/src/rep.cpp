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

#include "rep.hpp"
#include "err.hpp"

zmq::rep_t::rep_t (class ctx_t *parent_, uint32_t tid_) :
    xrep_t (parent_, tid_),
    sending_reply (false),
    request_begins (true)
{
    options.type = ZMQ_REP;
}

zmq::rep_t::~rep_t ()
{
}

int zmq::rep_t::xsend (zmq_msg_t *msg_, int flags_)
{
    //  If we are in the middle of receiving a request, we cannot send reply.
    if (!sending_reply) {
        errno = EFSM;
        return -1;
    }

    bool more = (msg_->flags & ZMQ_MSG_MORE);

    //  Push message to the reply pipe.
    int rc = xrep_t::xsend (msg_, flags_);
    if (rc != 0)
        return rc;

    //  If the reply is complete flip the FSM back to request receiving state.
    if (!more)
        sending_reply = false;

    return 0;
}

int zmq::rep_t::xrecv (zmq_msg_t *msg_, int flags_)
{
    //  If we are in middle of sending a reply, we cannot receive next request.
    if (sending_reply) {
        errno = EFSM;
        return -1;
    }

    if (request_begins) {

        //  Copy the backtrace stack to the reply pipe.
        bool bottom = false;
        while (!bottom) {

            //  TODO: What if request can be read but reply pipe is not
            //  ready for writing?

            //  Get next part of the backtrace stack.
            int rc = xrep_t::xrecv (msg_, flags_);
            if (rc != 0)
                return rc;

            if ((msg_->flags & ZMQ_MSG_MORE)) {
                //  Empty message part delimits the traceback stack.
                bottom = (zmq_msg_size (msg_) == 0);

                //  Push it to the reply pipe.
                rc = xrep_t::xsend (msg_, flags_);
                errno_assert (rc == 0);
            }
            else {
                //  If the traceback stack is malformed, discard anything
                //  already sent to pipe (we're at end of invalid message).
                rc = xrep_t::rollback ();
                zmq_assert (rc == 0);
            }
        }

        request_begins = false;
    }

    //  Now the routing info is safely stored. Get the first part
    //  of the message payload and exit.
    int rc = xrep_t::xrecv (msg_, flags_);
    if (rc != 0)
        return rc;

    //  If whole request is read, flip the FSM to reply-sending state.
    if (!(msg_->flags & ZMQ_MSG_MORE)) {
        sending_reply = true;
        request_begins = true;
    }

    return 0;
}

bool zmq::rep_t::xhas_in ()
{
    if (sending_reply)
        return false;

    return xrep_t::xhas_in ();
}

bool zmq::rep_t::xhas_out ()
{
    if (!sending_reply)
        return false;

    return xrep_t::xhas_out ();
}

