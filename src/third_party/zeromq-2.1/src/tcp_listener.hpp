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

#ifndef __ZMQ_TCP_LISTENER_HPP_INCLUDED__
#define __ZMQ_TCP_LISTENER_HPP_INCLUDED__

#include "fd.hpp"
#include "ip.hpp"

namespace zmq
{

    //  The class encapsulating simple TCP listening socket.

    class tcp_listener_t
    {
    public:

        tcp_listener_t ();
        ~tcp_listener_t ();

        //  Start listening on the interface.
        int set_address (const char *protocol_, const char *addr_,
            int backlog_);

        //  Close the listening socket.
        int close ();

        //  Get the file descriptor to poll on to get notified about
        //  newly created connections.
        fd_t get_fd ();

        //  Accept the new connection. Returns the file descriptor of the
        //  newly created connection. The function may return retired_fd
        //  if the connection was dropped while waiting in the listen backlog.
        fd_t accept ();

    private:

        //  Address to listen on.
        sockaddr_storage addr;
        socklen_t addr_len;

        //  True, if the undelying file for UNIX domain socket exists.
        bool has_file;

        //  Underlying socket.
        fd_t s;

        tcp_listener_t (const tcp_listener_t&);
        const tcp_listener_t &operator = (const tcp_listener_t&);
    };

}

#endif
