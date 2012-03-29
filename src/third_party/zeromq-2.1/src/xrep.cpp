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

#include "xrep.hpp"
#include "err.hpp"
#include "pipe.hpp"

zmq::xrep_t::xrep_t (class ctx_t *parent_, uint32_t tid_) :
    socket_base_t (parent_, tid_),
    current_in (0),
    prefetched (false),
    more_in (false),
    current_out (NULL),
    more_out (false),
    terminating (false)
{
    options.type = ZMQ_XREP;
    options.requires_in = true;
    options.requires_out = true;

    //  On connect, pipes are created only after initial handshaking.
    //  That way we are aware of the peer's identity when binding to the pipes.
    options.immediate_connect = false;
}

zmq::xrep_t::~xrep_t ()
{
    zmq_assert (inpipes.empty ());
    zmq_assert (outpipes.empty ());
}

void zmq::xrep_t::xattach_pipes (reader_t *inpipe_, writer_t *outpipe_,
    const blob_t &peer_identity_)
{
    if (outpipe_) {

        outpipe_->set_event_sink (this);

        //  TODO: What if new connection has same peer identity as the old one?
        outpipe_t outpipe = {outpipe_, true};
        bool ok = outpipes.insert (outpipes_t::value_type (
            peer_identity_, outpipe)).second;
        zmq_assert (ok);

        if (terminating) {
            register_term_acks (1);
            outpipe_->terminate ();
        }
    }

    if (inpipe_) {

        inpipe_->set_event_sink (this);

        inpipe_t inpipe = {inpipe_, peer_identity_, true};
        inpipes.push_back (inpipe);

        if (terminating) {
            register_term_acks (1);
            inpipe_->terminate ();
        }
    }
}

void zmq::xrep_t::process_term (int linger_)
{
    terminating = true;

    register_term_acks (inpipes.size () + outpipes.size ());

    for (inpipes_t::iterator it = inpipes.begin (); it != inpipes.end ();
          ++it)
        it->reader->terminate ();
    for (outpipes_t::iterator it = outpipes.begin (); it != outpipes.end ();
          ++it)
        it->second.writer->terminate ();

    socket_base_t::process_term (linger_);
}

void zmq::xrep_t::terminated (reader_t *pipe_)
{
    for (inpipes_t::iterator it = inpipes.begin (); it != inpipes.end ();
          ++it) {
        if (it->reader == pipe_) {
            if ((inpipes_t::size_type) (it - inpipes.begin ()) < current_in)
                current_in--;
            inpipes.erase (it);
            if (current_in >= inpipes.size ())
                current_in = 0;
            if (terminating)
                unregister_term_ack ();
            return;
        }
    }
    zmq_assert (false);
}

void zmq::xrep_t::terminated (writer_t *pipe_)
{
    for (outpipes_t::iterator it = outpipes.begin ();
          it != outpipes.end (); ++it) {
        if (it->second.writer == pipe_) {
            outpipes.erase (it);
            if (pipe_ == current_out)
                current_out = NULL;
            if (terminating)
                unregister_term_ack ();
            return;
        }
    }
    zmq_assert (false);
}

void zmq::xrep_t::delimited (reader_t *pipe_)
{
}

void zmq::xrep_t::activated (reader_t *pipe_)
{
    for (inpipes_t::iterator it = inpipes.begin (); it != inpipes.end ();
          ++it) {
        if (it->reader == pipe_) {
            zmq_assert (!it->active);
            it->active = true;
            return;
        }
    }
    zmq_assert (false);
}

void zmq::xrep_t::activated (writer_t *pipe_)
{
    for (outpipes_t::iterator it = outpipes.begin ();
          it != outpipes.end (); ++it) {
        if (it->second.writer == pipe_) {
            zmq_assert (!it->second.active);
            it->second.active = true;
            return;
        }
    }
    zmq_assert (false);
}

int zmq::xrep_t::xsend (zmq_msg_t *msg_, int flags_)
{
    //  If this is the first part of the message it's the identity of the
    //  peer to send the message to.
    if (!more_out) {
        zmq_assert (!current_out);

        //  If we have malformed message (prefix with no subsequent message)
        //  then just silently ignore it.
        if (msg_->flags & ZMQ_MSG_MORE) {

            more_out = true;

            //  Find the pipe associated with the identity stored in the prefix.
            //  If there's no such pipe just silently ignore the message.
            blob_t identity ((unsigned char*) zmq_msg_data (msg_),
                zmq_msg_size (msg_));
            outpipes_t::iterator it = outpipes.find (identity);

            if (it != outpipes.end ()) {
                current_out = it->second.writer;
                zmq_msg_t empty;
                int rc = zmq_msg_init (&empty);
                zmq_assert (rc == 0);
                if (!current_out->check_write (&empty)) {
                    it->second.active = false;
                    more_out = false;
                    current_out = NULL;
                }
                rc = zmq_msg_close (&empty);
                zmq_assert (rc == 0);
            }
        }

        int rc = zmq_msg_close (msg_);
        zmq_assert (rc == 0);
        rc = zmq_msg_init (msg_);
        zmq_assert (rc == 0);
        return 0;
    }

    //  Check whether this is the last part of the message.
    more_out = msg_->flags & ZMQ_MSG_MORE;

    //  Push the message into the pipe. If there's no out pipe, just drop it.
    if (current_out) {
        bool ok = current_out->write (msg_);
        zmq_assert (ok);
        if (!more_out) {
            current_out->flush ();
            current_out = NULL;
        }
    }
    else {
        int rc = zmq_msg_close (msg_);
        zmq_assert (rc == 0);
    }
    //  Detach the message from the data buffer.
    int rc = zmq_msg_init (msg_);
    zmq_assert (rc == 0);

    return 0;
}

int zmq::xrep_t::xrecv (zmq_msg_t *msg_, int flags_)
{
    //  If there is a prefetched message, return it.
    if (prefetched) {
        zmq_msg_move (msg_, &prefetched_msg);
        more_in = msg_->flags & ZMQ_MSG_MORE;
        prefetched = false;
        return 0;
    }

    //  Deallocate old content of the message.
    zmq_msg_close (msg_);

    //  If we are in the middle of reading a message, just grab next part of it.
    if (more_in) {
        zmq_assert (inpipes [current_in].active);
        bool fetched = inpipes [current_in].reader->read (msg_);
        zmq_assert (fetched);
        more_in = msg_->flags & ZMQ_MSG_MORE;
        if (!more_in) {
            current_in++;
            if (current_in >= inpipes.size ())
                current_in = 0;
        }
        return 0;
    }

    //  Round-robin over the pipes to get the next message.
    for (int count = inpipes.size (); count != 0; count--) {

        //  Try to fetch new message.
        if (inpipes [current_in].active)
            prefetched = inpipes [current_in].reader->read (&prefetched_msg);

        //  If we have a message, create a prefix and return it to the caller.
        if (prefetched) {
            int rc = zmq_msg_init_size (msg_,
                inpipes [current_in].identity.size ());
            zmq_assert (rc == 0);
            memcpy (zmq_msg_data (msg_), inpipes [current_in].identity.data (),
                zmq_msg_size (msg_));
            msg_->flags |= ZMQ_MSG_MORE;
            return 0;
        }

        //  If me don't have a message, mark the pipe as passive and
        //  move to next pipe.
        inpipes [current_in].active = false;
        current_in++;
        if (current_in >= inpipes.size ())
            current_in = 0;
    }

    //  No message is available. Initialise the output parameter
    //  to be a 0-byte message.
    zmq_msg_init (msg_);
    errno = EAGAIN;
    return -1;
}

int zmq::xrep_t::rollback (void)
{
    if (current_out) {
        current_out->rollback ();
        current_out = NULL;
        more_out = false;
    }
    return 0;
}

bool zmq::xrep_t::xhas_in ()
{
    //  There are subsequent parts of the partly-read message available.
    if (prefetched || more_in)
        return true;

    //  Note that messing with current doesn't break the fairness of fair
    //  queueing algorithm. If there are no messages available current will
    //  get back to its original value. Otherwise it'll point to the first
    //  pipe holding messages, skipping only pipes with no messages available.
    for (int count = inpipes.size (); count != 0; count--) {
        if (inpipes [current_in].active &&
              inpipes [current_in].reader->check_read ())
            return true;

        //  If me don't have a message, mark the pipe as passive and
        //  move to next pipe.
        inpipes [current_in].active = false;
        current_in++;
        if (current_in >= inpipes.size ())
            current_in = 0;
    }

    return false;
}

bool zmq::xrep_t::xhas_out ()
{
    //  In theory, XREP socket is always ready for writing. Whether actual
    //  attempt to write succeeds depends on whitch pipe the message is going
    //  to be routed to.
    return true;
}


