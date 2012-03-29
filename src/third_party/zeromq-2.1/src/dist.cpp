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

#include "dist.hpp"
#include "pipe.hpp"
#include "err.hpp"
#include "own.hpp"
#include "msg_content.hpp"
#include "likely.hpp"

zmq::dist_t::dist_t (own_t *sink_) :
    active (0),
    eligible (0),
    more (false),
    sink (sink_),
    terminating (false)
{
}

zmq::dist_t::~dist_t ()
{
    zmq_assert (pipes.empty ());
}

void zmq::dist_t::attach (writer_t *pipe_)
{
    pipe_->set_event_sink (this);

    //  If we are in the middle of sending a message, we'll add new pipe
    //  into the list of eligible pipes. Otherwise we add it to the list
    //  of active pipes.
    if (more) {
        pipes.push_back (pipe_);
        pipes.swap (eligible, pipes.size () - 1);
        eligible++;
    }
    else {
        pipes.push_back (pipe_);
        pipes.swap (active, pipes.size () - 1);
        active++;
        eligible++;
    }

    if (unlikely (terminating)) {
        sink->register_term_acks (1);
        pipe_->terminate ();
    }
}

void zmq::dist_t::terminate ()
{
    zmq_assert (!terminating);
    terminating = true;

    sink->register_term_acks (pipes.size ());
    for (pipes_t::size_type i = 0; i != pipes.size (); i++)
        pipes [i]->terminate ();
}

void zmq::dist_t::terminated (writer_t *pipe_)
{
    //  Remove the pipe from the list; adjust number of active and/or
    //  eligible pipes accordingly.
    if (pipes.index (pipe_) < active)
        active--;
    if (pipes.index (pipe_) < eligible)
        eligible--;
    pipes.erase (pipe_);

    if (unlikely (terminating))
        sink->unregister_term_ack ();
}

void zmq::dist_t::activated (writer_t *pipe_)
{
    //  Move the pipe from passive to eligible state.
    pipes.swap (pipes.index (pipe_), eligible);
    eligible++;

    //  If there's no message being sent at the moment, move it to
    //  the active state.
    if (!more) {
        pipes.swap (eligible - 1, active);
        active++;
    }
}

int zmq::dist_t::send (zmq_msg_t *msg_, int flags_)
{
    //  Is this end of a multipart message?
    bool msg_more = msg_->flags & ZMQ_MSG_MORE;

    //  Push the message to active pipes.
    distribute (msg_, flags_);

    //  If multipart message is fully sent, activate all the eligible pipes.
    if (!msg_more)
        active = eligible;

    more = msg_more;

    return 0;
}

void zmq::dist_t::distribute (zmq_msg_t *msg_, int flags_)
{
    //  If there are no active pipes available, simply drop the message.
    if (active == 0) {
        int rc = zmq_msg_close (msg_);
        zmq_assert (rc == 0);
        rc = zmq_msg_init (msg_);
        zmq_assert (rc == 0);
        return;
    }

    msg_content_t *content = (msg_content_t*) msg_->content;

    //  For VSMs the copying is straighforward.
    if (content == (msg_content_t*) ZMQ_VSM) {
        for (pipes_t::size_type i = 0; i < active;)
            if (write (pipes [i], msg_))
                i++;
        int rc = zmq_msg_init (msg_);
        zmq_assert (rc == 0);
        return;
    }

    //  Optimisation for the case when there's only a single pipe
    //  to send the message to - no refcount adjustment i.e. no atomic
    //  operations are needed.
    if (active == 1) {
        if (!write (pipes [0], msg_)) {
            int rc = zmq_msg_close (msg_);
            zmq_assert (rc == 0);
        }
        int rc = zmq_msg_init (msg_);
        zmq_assert (rc == 0);
        return;
    }

    //  There are at least 2 destinations for the message. That means we have
    //  to deal with reference counting. First add N-1 references to
    //  the content (we are holding one reference anyway, that's why -1).
    if (msg_->flags & ZMQ_MSG_SHARED)
        content->refcnt.add (active - 1);
    else {
        content->refcnt.set (active);
        msg_->flags |= ZMQ_MSG_SHARED;
    }

    //  Push the message to all destinations.
    for (pipes_t::size_type i = 0; i < active;) {
        if (!write (pipes [i], msg_))
            content->refcnt.sub (1);
        else
            i++;
    }

    //  Detach the original message from the data buffer.
    int rc = zmq_msg_init (msg_);
    zmq_assert (rc == 0);
}

bool zmq::dist_t::has_out ()
{
    return true;
}

bool zmq::dist_t::write (class writer_t *pipe_, zmq_msg_t *msg_)
{
    if (!pipe_->write (msg_)) {
        pipes.swap (pipes.index (pipe_), active - 1);
        active--;
        pipes.swap (active, eligible - 1);
        eligible--;
        return false;
    }
    if (!(msg_->flags & ZMQ_MSG_MORE))
        pipe_->flush ();
    return true;
}
