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

#ifndef __ZMQ_SWAP_HPP_INCLUDED__
#define __ZMQ_SWAP_HPP_INCLUDED__

#include "../include/zmq.h"

#include <string>
#include "stdint.hpp"

namespace zmq
{

    //  This class implements a message swap. Messages are retrieved from
    //  the swap in the same order as they entered it.

    class swap_t
    {
    public:

        enum { default_block_size = 8192 };

        //  Creates the swap.
        swap_t (int64_t filesize_);

        ~swap_t ();

        int init ();

        //  Stores the message into the swap. The function
        //  returns false if the swap is full; true otherwise.
        bool store (zmq_msg_t *msg_);

        //  Fetches the oldest message from the swap. It is an error
        //  to call this function when the swap is empty.
        void fetch (zmq_msg_t *msg_);

        void commit ();

        void rollback ();

        //  Returns true if the swap is empty; false otherwise.
        bool empty ();


//        //  Returns true if and only if the swap is full.
//        bool full ();

        //  Returns true if the message fits into swap.
        bool fits (zmq_msg_t *msg_);

    private:

        //  Copies data from a memory buffer to the backing file.
        //  Wraps around when reaching maximum file size.
        void copy_from_file (void *buffer_, size_t count_);

        //  Copies data from the backing file to the memory buffer.
        //  Wraps around when reaching end-of-file.
        void copy_to_file (const void *buffer_, size_t count_);

        //  Returns the buffer space available.
        int64_t buffer_space ();

        void fill_buf (char *buf, int64_t pos);

        void save_write_buf ();

        //  File descriptor to the backing file.
        int fd;

        //  Name of the backing file.
        std::string filename;

        //  Maximum size of the backing file.
        int64_t filesize;

        //  File offset associated with the fd file descriptor.
        int64_t file_pos;

        //  File offset the next message will be stored at.
        int64_t write_pos;

        //  File offset the next message will be read from.
        int64_t read_pos;

        int64_t commit_pos;

        size_t block_size;

        char *buf1;
        char *buf2;
        char *read_buf;
        char *write_buf;

        int64_t write_buf_start_addr;

        //  Disable copying of the swap object.
        swap_t (const swap_t&);
        const swap_t &operator = (const swap_t&);
    };

}

#endif
