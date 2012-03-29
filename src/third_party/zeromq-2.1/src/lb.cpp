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

#include "lb.hpp"
#include "pipe.hpp"
#include "err.hpp"
#include "own.hpp"

zmq::lb_t::lb_t (own_t *sink_) :
    active (0),
    current (0),
    more (false),
    dropping (false),
    sink (sink_),
    terminating (false)
{
}

zmq::lb_t::~lb_t ()
{
    zmq_assert (pipes.empty ());
}

void zmq::lb_t::attach (writer_t *pipe_)
{
    pipe_->set_event_sink (this);

    pipes.push_back (pipe_);
    pipes.swap (active, pipes.size () - 1);
    active++;

    if (terminating) {
        sink->register_term_acks (1);
        pipe_->terminate ();
    }
}

void zmq::lb_t::terminate ()
{
    zmq_assert (!terminating);
    terminating = true;

    sink->register_term_acks (pipes.size ());
    for (pipes_t::size_type i = 0; i != pipes.size (); i++)
        pipes [i]->terminate ();
}

void zmq::lb_t::terminated (writer_t *pipe_)
{
    pipes_t::size_type index = pipes.index (pipe_);

    //  If we are in the middle of multipart message and current pipe
    //  have disconnected, we have to drop the remainder of the message.
    if (index == current && more)
        dropping = true;

    //  Remove the pipe from the list; adjust number of active pipes
    //  accordingly.
    if (index < active) {
        active--;
        if (current == active)
            current = 0;
    }
    pipes.erase (pipe_);

    if (terminating)
        sink->unregister_term_ack ();
}

void zmq::lb_t::activated (writer_t *pipe_)
{
    //  Move the pipe to the list of active pipes.
    pipes.swap (pipes.index (pipe_), active);
    active++;
}

int zmq::lb_t::send (zmq_msg_t *msg_, int flags_)
{
    //  Drop the message if required. If we are at the end of the message
    //  switch back to non-dropping mode.
    if (dropping) {

        more = msg_->flags & ZMQ_MSG_MORE;
        if (!more)
            dropping = false;

        int rc = zmq_msg_close (msg_);
        errno_assert (rc == 0);
        rc = zmq_msg_init (msg_);
        zmq_assert (rc == 0);
        return 0;
    }

    while (active > 0) {
        if (pipes [current]->write (msg_)) {
            more = msg_->flags & ZMQ_MSG_MORE;
            break;
        }

        zmq_assert (!more);
        active--;
        if (current < active)
            pipes.swap (current, active);
        else
            current = 0;
    }

    //  If there are no pipes we cannot send the message.
    if (active == 0) {
        errno = EAGAIN;
        return -1;
    }

    //  If it's final part of the message we can fluch it downstream and
    //  continue round-robinning (load balance).
    if (!more) {
        pipes [current]->flush ();
        current = (current + 1) % active;
    }

    //  Detach the message from the data buffer.
    int rc = zmq_msg_init (msg_);
    zmq_assert (rc == 0);

    return 0;
}

bool zmq::lb_t::has_out ()
{
    //  If one part of the message was already written we can definitely
    //  write the rest of the message.
    if (more)
        return true;

    while (active > 0) {

        //  Check whether zero-sized message can be written to the pipe.
        zmq_msg_t msg;
        zmq_msg_init (&msg);
        if (pipes [current]->check_write (&msg)) {
            zmq_msg_close (&msg);
            return true;
        }
        zmq_msg_close (&msg);

        //  Deactivate the pipe.
        active--;
        pipes.swap (current, active);
        if (current == active)
            current = 0;
    }

    return false;
}

