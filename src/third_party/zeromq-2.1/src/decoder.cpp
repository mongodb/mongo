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

#include <stdlib.h>
#include <string.h>

#include "decoder.hpp"
#include "i_inout.hpp"
#include "wire.hpp"
#include "err.hpp"

zmq::decoder_t::decoder_t (size_t bufsize_) :
    decoder_base_t <decoder_t> (bufsize_),
    destination (NULL)
{
    zmq_msg_init (&in_progress);

    //  At the beginning, read one byte and go to one_byte_size_ready state.
    next_step (tmpbuf, 1, &decoder_t::one_byte_size_ready);
}

zmq::decoder_t::~decoder_t ()
{
    zmq_msg_close (&in_progress);
}

void zmq::decoder_t::set_inout (i_inout *destination_)
{
    destination = destination_;
}

bool zmq::decoder_t::one_byte_size_ready ()
{
    //  First byte of size is read. If it is 0xff read 8-byte size.
    //  Otherwise allocate the buffer for message data and read the
    //  message data into it.
    if (*tmpbuf == 0xff)
        next_step (tmpbuf, 8, &decoder_t::eight_byte_size_ready);
    else {

        //  There has to be at least one byte (the flags) in the message).
        if (!*tmpbuf) {
            decoding_error ();
            return false;
        }

        //  in_progress is initialised at this point so in theory we should
        //  close it before calling zmq_msg_init_size, however, it's a 0-byte
        //  message and thus we can treat it as uninitialised...
        int rc = zmq_msg_init_size (&in_progress, *tmpbuf - 1);
        if (rc != 0 && errno == ENOMEM) {
            rc = zmq_msg_init (&in_progress);
            errno_assert (rc == 0);
            decoding_error ();
            return false;
        }
        errno_assert (rc == 0);

        next_step (tmpbuf, 1, &decoder_t::flags_ready);
    }
    return true;
}

bool zmq::decoder_t::eight_byte_size_ready ()
{
    //  8-byte size is read. Allocate the buffer for message body and
    //  read the message data into it.
    size_t size = (size_t) get_uint64 (tmpbuf);

    //  There has to be at least one byte (the flags) in the message).
    if (!size) {
        decoding_error ();
        return false;
    }

    //  in_progress is initialised at this point so in theory we should
    //  close it before calling zmq_msg_init_size, however, it's a 0-byte
    //  message and thus we can treat it as uninitialised...
    int rc = zmq_msg_init_size (&in_progress, size - 1);
    if (rc != 0 && errno == ENOMEM) {
        rc = zmq_msg_init (&in_progress);
        errno_assert (rc == 0);
        decoding_error ();
        return false;
    }
    errno_assert (rc == 0);

    next_step (tmpbuf, 1, &decoder_t::flags_ready);
    return true;
}

bool zmq::decoder_t::flags_ready ()
{
    //  Store the flags from the wire into the message structure.
    in_progress.flags = tmpbuf [0] | ~ZMQ_MSG_MASK;

    next_step (zmq_msg_data (&in_progress), zmq_msg_size (&in_progress),
        &decoder_t::message_ready);

    return true;
}

bool zmq::decoder_t::message_ready ()
{
    //  Message is completely read. Push it further and start reading
    //  new message. (in_progress is a 0-byte message after this point.)
    if (!destination || !destination->write (&in_progress))
        return false;

    next_step (tmpbuf, 1, &decoder_t::one_byte_size_ready);
    return true;
}
