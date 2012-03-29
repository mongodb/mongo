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

#ifndef __ZMQ_ENCODER_HPP_INCLUDED__
#define __ZMQ_ENCODER_HPP_INCLUDED__

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>

#include "err.hpp"

#include "../include/zmq.h"

namespace zmq
{

    //  Helper base class for encoders. It implements the state machine that
    //  fills the outgoing buffer. Derived classes should implement individual
    //  state machine actions.

    template <typename T> class encoder_base_t
    {
    public:

        inline encoder_base_t (size_t bufsize_) :
            bufsize (bufsize_)
        {
            buf = (unsigned char*) malloc (bufsize_);
            alloc_assert (buf);
        }

        //  The destructor doesn't have to be virtual. It is made virtual
        //  just to keep ICC and code checking tools from complaining.
        inline virtual ~encoder_base_t ()
        {
            free (buf);
        }

        //  The function returns a batch of binary data. The data
        //  are filled to a supplied buffer. If no buffer is supplied (data_
        //  points to NULL) decoder object will provide buffer of its own.
        //  If offset is not NULL, it is filled by offset of the first message
        //  in the batch.If there's no beginning of a message in the batch,
        //  offset is set to -1.
        inline void get_data (unsigned char **data_, size_t *size_,
            int *offset_ = NULL)
        {
            unsigned char *buffer = !*data_ ? buf : *data_;
            size_t buffersize = !*data_ ? bufsize : *size_;

            size_t pos = 0;
            if (offset_)
                *offset_ = -1;

            while (true) {

                //  If there are no more data to return, run the state machine.
                //  If there are still no data, return what we already have
                //  in the buffer.
                if (!to_write) {
                    if (!(static_cast <T*> (this)->*next) ()) {
                        *data_ = buffer;
                        *size_ = pos;
                        return;
                    }

                    //  If beginning of the message was processed, adjust the
                    //  first-message-offset.
                    if (beginning) { 
                        if (offset_ && *offset_ == -1)
                            *offset_ = pos;
                        beginning = false;
                    }
                }

                //  If there are no data in the buffer yet and we are able to
                //  fill whole buffer in a single go, let's use zero-copy.
                //  There's no disadvantage to it as we cannot stuck multiple
                //  messages into the buffer anyway. Note that subsequent
                //  write(s) are non-blocking, thus each single write writes
                //  at most SO_SNDBUF bytes at once not depending on how large
                //  is the chunk returned from here.
                //  As a consequence, large messages being sent won't block
                //  other engines running in the same I/O thread for excessive
                //  amounts of time.
                if (!pos && !*data_ && to_write >= buffersize) {
                    *data_ = write_pos;
                    *size_ = to_write;
                    write_pos = NULL;
                    to_write = 0;
                    return;
                }

                //  Copy data to the buffer. If the buffer is full, return.
                size_t to_copy = std::min (to_write, buffersize - pos);
                memcpy (buffer + pos, write_pos, to_copy);
                pos += to_copy;
                write_pos += to_copy;
                to_write -= to_copy;
                if (pos == buffersize) {
                    *data_ = buffer;
                    *size_ = pos;
                    return;
                }
            }
        }

    protected:

        //  Prototype of state machine action.
        typedef bool (T::*step_t) ();

        //  This function should be called from derived class to write the data
        //  to the buffer and schedule next state machine action. Set beginning
        //  to true when you are writing first byte of a message.
        inline void next_step (void *write_pos_, size_t to_write_,
            step_t next_, bool beginning_)
        {
            write_pos = (unsigned char*) write_pos_;
            to_write = to_write_;
            next = next_;
            beginning = beginning_;
        }

    private:

        unsigned char *write_pos;
        size_t to_write;
        step_t next;
        bool beginning;

        size_t bufsize;
        unsigned char *buf;

        encoder_base_t (const encoder_base_t&);
        void operator = (const encoder_base_t&);
    };

    //  Encoder for 0MQ framing protocol. Converts messages into data batches.

    class encoder_t : public encoder_base_t <encoder_t>
    {
    public:

        encoder_t (size_t bufsize_);
        ~encoder_t ();

        void set_inout (struct i_inout *source_);

    private:

        bool size_ready ();
        bool message_ready ();

        struct i_inout *source;
        ::zmq_msg_t in_progress;
        unsigned char tmpbuf [10];

        encoder_t (const encoder_t&);
        const encoder_t &operator = (const encoder_t&);
    };
}

#endif

