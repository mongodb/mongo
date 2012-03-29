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

#ifndef __ZMQ_TCP_SOCKET_HPP_INCLUDED__
#define __ZMQ_TCP_SOCKET_HPP_INCLUDED__

#include "fd.hpp"
#include "stdint.hpp"

namespace zmq
{

    //  The class encapsulating simple TCP read/write socket.

    class tcp_socket_t
    {
    public:

        tcp_socket_t ();
        ~tcp_socket_t ();

        //  Associates a socket with a native socket descriptor.
        int open (fd_t fd_, uint64_t sndbuf_, uint64_t rcvbuf_);
         
        //  Closes the underlying socket.
        int close ();

        //  Returns the underlying socket. Returns retired_fd when the socket
        //  is in the closed state.
        fd_t get_fd ();

        //  Writes data to the socket. Returns the number of bytes actually
        //  written (even zero is to be considered to be a success). In case
        //  of error or orderly shutdown by the other peer -1 is returned.
        int write (const void *data, int size);

        //  Reads data from the socket (up to 'size' bytes). Returns the number
        //  of bytes actually read (even zero is to be considered to be
        //  a success). In case of error or orderly shutdown by the other
        //  peer -1 is returned.
        int read (void *data, int size);

    private:

        //  Underlying socket.
        fd_t s;

        //  Disable copy construction of tcp_socket.
        tcp_socket_t (const tcp_socket_t&);
        const tcp_socket_t &operator = (const tcp_socket_t&);
    };

}

#endif
