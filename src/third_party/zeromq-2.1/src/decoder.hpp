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

#ifndef __ZMQ_DECODER_HPP_INCLUDED__
#define __ZMQ_DECODER_HPP_INCLUDED__

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>

#include "err.hpp"

#include "../include/zmq.h"

namespace zmq
{

    //  Helper base class for decoders that know the amount of data to read
    //  in advance at any moment. Knowing the amount in advance is a property
    //  of the protocol used. 0MQ framing protocol is based size-prefixed
    //  paradigm, whixh qualifies it to be parsed by this class.
    //  On the other hand, XML-based transports (like XMPP or SOAP) don't allow
    //  for knowing the size of data to read in advance and should use different
    //  decoding algorithms.
    //
    //  This class implements the state machine that parses the incoming buffer.
    //  Derived class should implement individual state machine actions.

    template <typename T> class decoder_base_t
    {
    public:

        inline decoder_base_t (size_t bufsize_) :
            read_pos (NULL),
            to_read (0),
            next (NULL),
            bufsize (bufsize_)
        {
            buf = (unsigned char*) malloc (bufsize_);
            alloc_assert (buf);
        }

        //  The destructor doesn't have to be virtual. It is mad virtual
        //  just to keep ICC and code checking tools from complaining.
        inline virtual ~decoder_base_t ()
        {
            free (buf);
        }

        //  Returns a buffer to be filled with binary data.
        inline void get_buffer (unsigned char **data_, size_t *size_)
        {
            //  If we are expected to read large message, we'll opt for zero-
            //  copy, i.e. we'll ask caller to fill the data directly to the
            //  message. Note that subsequent read(s) are non-blocking, thus
            //  each single read reads at most SO_RCVBUF bytes at once not
            //  depending on how large is the chunk returned from here.
            //  As a consequence, large messages being received won't block
            //  other engines running in the same I/O thread for excessive
            //  amounts of time.
            if (to_read >= bufsize) {
                *data_ = read_pos;
                *size_ = to_read;
                return;
            }

            *data_ = buf;
            *size_ = bufsize;
        }

        //  Processes the data in the buffer previously allocated using
        //  get_buffer function. size_ argument specifies nemuber of bytes
        //  actually filled into the buffer. Function returns number of
        //  bytes actually processed.
        inline size_t process_buffer (unsigned char *data_, size_t size_)
        {
            //  Check if we had an error in previous attempt.
            if (unlikely (!(static_cast <T*> (this)->next)))
                return (size_t) -1;

            //  In case of zero-copy simply adjust the pointers, no copying
            //  is required. Also, run the state machine in case all the data
            //  were processed.
            if (data_ == read_pos) {
                read_pos += size_;
                to_read -= size_;

                while (!to_read) {
                    if (!(static_cast <T*> (this)->*next) ()) {
                        if (unlikely (!(static_cast <T*> (this)->next)))
                            return (size_t) -1;
                        return size_;
                    }
                }
                return size_;
            }

            size_t pos = 0;
            while (true) {

                //  Try to get more space in the message to fill in.
                //  If none is available, return.
                while (!to_read) {
                    if (!(static_cast <T*> (this)->*next) ()) {
                        if (unlikely (!(static_cast <T*> (this)->next)))
                            return (size_t) -1;
                        return pos;
                    }
                }

                //  If there are no more data in the buffer, return.
                if (pos == size_)
                    return pos;

                //  Copy the data from buffer to the message.
                size_t to_copy = std::min (to_read, size_ - pos);
                memcpy (read_pos, data_ + pos, to_copy);
                read_pos += to_copy;
                pos += to_copy;
                to_read -= to_copy;
            }
        }

    protected:

        //  Prototype of state machine action. Action should return false if
        //  it is unable to push the data to the system.
        typedef bool (T::*step_t) ();

        //  This function should be called from derived class to read data
        //  from the buffer and schedule next state machine action.
        inline void next_step (void *read_pos_, size_t to_read_,
            step_t next_)
        {
            read_pos = (unsigned char*) read_pos_;
            to_read = to_read_;
            next = next_;
        }

        //  This function should be called from the derived class to
        //  abort decoder state machine.
        inline void decoding_error ()
        {
            next = NULL;
        }

    private:

        unsigned char *read_pos;
        size_t to_read;
        step_t next;

        size_t bufsize;
        unsigned char *buf;

        decoder_base_t (const decoder_base_t&);
        const decoder_base_t &operator = (const decoder_base_t&);
    };

    //  Decoder for 0MQ framing protocol. Converts data batches into messages.

    class decoder_t : public decoder_base_t <decoder_t>
    {
    public:

        decoder_t (size_t bufsize_);
        ~decoder_t ();

        void set_inout (struct i_inout *destination_);

    private:

        bool one_byte_size_ready ();
        bool eight_byte_size_ready ();
        bool flags_ready ();
        bool message_ready ();

        struct i_inout *destination;
        unsigned char tmpbuf [8];
        ::zmq_msg_t in_progress;

        decoder_t (const decoder_t&);
        void operator = (const decoder_t&);
    };

}

#endif

