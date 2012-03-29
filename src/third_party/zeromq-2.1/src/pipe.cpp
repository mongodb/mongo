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

#include <new>

#include "../include/zmq.h"

#include "pipe.hpp"
#include "likely.hpp"

zmq::reader_t::reader_t (object_t *parent_, pipe_t *pipe_,
      uint64_t lwm_) :
    object_t (parent_),
    active (true),
    pipe (pipe_),
    writer (NULL),
    lwm (lwm_),
    msgs_read (0),
    sink (NULL),
    terminating (false)
{
    //  Note that writer is not set here. Writer will inform reader about its
    //  address once it is created (via set_writer method).
}

void zmq::reader_t::set_writer (writer_t *writer_)
{
    zmq_assert (!writer);
    writer = writer_;
}

zmq::reader_t::~reader_t ()
{
    //  Pipe as such is owned and deallocated by reader object.
    //  The point is that reader processes the last step of terminal
    //  handshaking (term_ack).
    zmq_assert (pipe);

    //  First delete all the unread messages in the pipe. We have to do it by
    //  hand because zmq_msg_t is a POD, not a class, so there's no associated
    //  destructor.
    zmq_msg_t msg;
    while (pipe->read (&msg))
       zmq_msg_close (&msg);

    delete pipe;
}

void zmq::reader_t::set_event_sink (i_reader_events *sink_)
{
    zmq_assert (!sink);
    sink = sink_;
}

bool zmq::reader_t::is_delimiter (zmq_msg_t &msg_)
{
    unsigned char *offset = 0;

    return msg_.content == (void*) (offset + ZMQ_DELIMITER);
}

bool zmq::reader_t::check_read ()
{
    if (!active)
        return false;

    //  Check if there's an item in the pipe.
    if (!pipe->check_read ()) {
        active = false;
        return false;
    }

    //  If the next item in the pipe is message delimiter,
    //  initiate its termination.
    if (pipe->probe (is_delimiter)) {
        zmq_msg_t msg;
        bool ok = pipe->read (&msg);
        zmq_assert (ok);
        if (sink)
            sink->delimited (this);
        terminate ();
        return false;
    }

    return true;
}

bool zmq::reader_t::read (zmq_msg_t *msg_)
{
    if (!active)
        return false;

    if (!pipe->read (msg_)) {
        active = false;
        return false;
    }

    //  If delimiter was read, start termination process of the pipe.
    unsigned char *offset = 0;
    if (msg_->content == (void*) (offset + ZMQ_DELIMITER)) {
        if (sink)
            sink->delimited (this);
        terminate ();
        return false;
    }

    if (!(msg_->flags & ZMQ_MSG_MORE))
        msgs_read++;

    if (lwm > 0 && msgs_read % lwm == 0)
        send_activate_writer (writer, msgs_read);

    return true;
}

void zmq::reader_t::terminate ()
{
    //  If termination was already started by the peer, do nothing.
    if (terminating)
        return;

    active = false;
    terminating = true;
    send_pipe_term (writer);
}

void zmq::reader_t::process_activate_reader ()
{
    //  Forward the event to the sink (either socket or session).
    active = true;
    sink->activated (this);
}

void zmq::reader_t::process_pipe_term_ack ()
{
    //  At this point writer may already be deallocated.
    //  For safety's sake drop the reference to it.
    writer = NULL;

    //  Notify owner about the termination.
    zmq_assert (sink);
    sink->terminated (this);

    //  Deallocate resources.
    delete this;
}

zmq::writer_t::writer_t (object_t *parent_, pipe_t *pipe_, reader_t *reader_,
      uint64_t hwm_, int64_t swap_size_) :
    object_t (parent_),
    active (true),
    pipe (pipe_),
    reader (reader_),
    hwm (hwm_),
    msgs_read (0),
    msgs_written (0),
    swap (NULL),
    sink (NULL),
    swapping (false),
    pending_delimiter (false),
    terminating (false)
{
    //  Inform reader about the writer.
    reader->set_writer (this);

    //  Open the swap file, if required.
    if (swap_size_ > 0) {
        swap = new (std::nothrow) swap_t (swap_size_);
        alloc_assert (swap);
        int rc = swap->init ();
        zmq_assert (rc == 0);
    }
}

zmq::writer_t::~writer_t ()
{
    if (swap)
       delete swap;
}

void zmq::writer_t::set_event_sink (i_writer_events *sink_)
{
    zmq_assert (!sink);
    sink = sink_;
}

bool zmq::writer_t::check_write (zmq_msg_t *msg_)
{
    //  We've already checked and there's no space free for the new message.
    //  There's no point in checking once again.
    if (unlikely (!active))
        return false;

    if (unlikely (swapping)) {
        if (unlikely (!swap->fits (msg_))) {
            active = false;
            return false;
        }
    }
    else {
        if (unlikely (pipe_full ())) {
            if (swap)
                swapping = true;
            else {
                active = false;
                return false;
            }
        }
    }

    return true;
}

bool zmq::writer_t::write (zmq_msg_t *msg_)
{
    if (unlikely (!check_write (msg_)))
        return false;

    if (unlikely (swapping)) {
        bool stored = swap->store (msg_);
        zmq_assert (stored);
        if (!(msg_->flags & ZMQ_MSG_MORE))
            swap->commit ();
        return true;
    }

    pipe->write (*msg_, msg_->flags & ZMQ_MSG_MORE);
    if (!(msg_->flags & ZMQ_MSG_MORE))
        msgs_written++;

    return true;
}

void zmq::writer_t::rollback ()
{
    //  Remove incomplete message from the swap.
    if (unlikely (swapping)) {
        swap->rollback ();
        return;
    }

    //  Remove incomplete message from the pipe.
    zmq_msg_t msg;
    while (pipe->unwrite (&msg)) {
        zmq_assert (msg.flags & ZMQ_MSG_MORE);
        zmq_msg_close (&msg);
    }
}

void zmq::writer_t::flush ()
{
    //  In the swapping mode, flushing is automatically handled by swap object.
    if (!swapping && !pipe->flush ())
        send_activate_reader (reader);
}

void zmq::writer_t::terminate ()
{
    //  Prevent double termination.
    if (terminating)
        return;
    terminating = true;

    //  Mark the pipe as not available for writing.
    active = false;

    //  Rollback any unfinished messages.
    rollback ();

    if (swapping) {
        pending_delimiter = true;
        return;
    }

    //  Push delimiter into the pipe. Trick the compiler to belive that
    //  the tag is a valid pointer. Note that watermarks are not checked
    //  thus the delimiter can be written even though the pipe is full.
    zmq_msg_t msg;
    const unsigned char *offset = 0;
    msg.content = (void*) (offset + ZMQ_DELIMITER);
    msg.flags = 0;
    pipe->write (msg, false);
    flush ();
}

void zmq::writer_t::process_activate_writer (uint64_t msgs_read_)
{
    //  Store the reader's message sequence number.
    msgs_read = msgs_read_;

    //  If we are in the swapping mode, we have some messages in the swap.
    //  Given that pipe is now ready for writing we can move part of the
    //  swap into the pipe.
    if (swapping) {
        zmq_msg_t msg;
        while (!pipe_full () && !swap->empty ()) {
            swap->fetch(&msg);
            pipe->write (msg, msg.flags & ZMQ_MSG_MORE);
            if (!(msg.flags & ZMQ_MSG_MORE))
                msgs_written++;
        }
        if (!pipe->flush ())
            send_activate_reader (reader);

        //  There are no more messages in the swap. We can switch into
        //  standard in-memory mode.
        if (swap->empty ()) {        
            swapping = false;

            //  Push delimiter into the pipe. Trick the compiler to belive that
            //  the tag is a valid pointer. Note that watermarks are not checked
            //  thus the delimiter can be written even though the pipe is full.
            if (pending_delimiter) {
                zmq_msg_t msg;
                const unsigned char *offset = 0;
                msg.content = (void*) (offset + ZMQ_DELIMITER);
                msg.flags = 0;
                pipe->write (msg, false);
                flush ();
                return;
            }
        }
    }

    //  If the writer was non-active before, let's make it active
    //  (available for writing messages to).
    if (!active && !terminating) {
        active = true;
        zmq_assert (sink);
        sink->activated (this);
    }
}

void zmq::writer_t::process_pipe_term ()
{
    send_pipe_term_ack (reader);

    //  The above command allows reader to deallocate itself and the pipe.
    //  For safety's sake we'll drop the pointers here.
    reader = NULL;
    pipe = NULL;

    //  Notify owner about the termination.
    zmq_assert (sink);
    sink->terminated (this);

    //  Deallocate the resources.
    delete this;
}

bool zmq::writer_t::pipe_full ()
{
    return hwm > 0 && msgs_written - msgs_read == hwm;
}

void zmq::create_pipe (object_t *reader_parent_, object_t *writer_parent_,
    uint64_t hwm_, int64_t swap_size_, reader_t **reader_, writer_t **writer_)
{
    //  First compute the low water mark. Following point should be taken
    //  into consideration:
    //
    //  1. LWM has to be less than HWM.
    //  2. LWM cannot be set to very low value (such as zero) as after filling
    //     the queue it would start to refill only after all the messages are
    //     read from it and thus unnecessarily hold the progress back.
    //  3. LWM cannot be set to very high value (such as HWM-1) as it would
    //     result in lock-step filling of the queue - if a single message is
    //     read from a full queue, writer thread is resumed to write exactly one
    //     message to the queue and go back to sleep immediately. This would
    //     result in low performance.
    //
    //  Given the 3. it would be good to keep HWM and LWM as far apart as
    //  possible to reduce the thread switching overhead to almost zero,
    //  say HWM-LWM should be max_wm_delta.
    //
    //  That done, we still we have to account for the cases where
    //  HWM < max_wm_delta thus driving LWM to negative numbers.
    //  Let's make LWM 1/2 of HWM in such cases.
    uint64_t lwm = (hwm_ > max_wm_delta * 2) ?
        hwm_ - max_wm_delta : (hwm_ + 1) / 2;

    //  Create all three objects pipe consists of: the pipe per se, reader and
    //  writer. The pipe will be handled by reader and writer, its never passed
    //  to the user. Reader and writer are returned to the user.
    pipe_t *pipe = new (std::nothrow) pipe_t ();
    alloc_assert (pipe);
    *reader_ = new (std::nothrow) reader_t (reader_parent_, pipe, lwm);
    alloc_assert (*reader_);
    *writer_ = new (std::nothrow) writer_t (writer_parent_, pipe, *reader_,
        hwm_, swap_size_);
    alloc_assert (*writer_);
}
