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

#ifndef __ZMQ_TCP_CONNECTER_HPP_INCLUDED__
#define __ZMQ_TCP_CONNECTER_HPP_INCLUDED__

#include "platform.hpp"
#include "fd.hpp"

#ifdef ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <sys/types.h>
#include <sys/socket.h>
#endif

namespace zmq
{

    //  The class encapsulating simple TCP listening socket.

    class tcp_connecter_t
    {
    public:

        tcp_connecter_t ();
        ~tcp_connecter_t ();

        //  Set address to connect to.
        int set_address (const char *protocol, const char *addr_);

        //  Open TCP connecting socket. Address is in
        //  <hostname>:<port-number> format. Returns -1 in case of error,
        //  0 if connect was successfull immediately and 1 if async connect
        //  was launched.
        int open ();

        //  Close the connecting socket.
        int close ();

        //  Get the file descriptor to poll on to get notified about
        //  connection success.
        fd_t get_fd ();

        //  Get the file descriptor of newly created connection. Returns
        //  retired_fd if the connection was unsuccessfull.
        fd_t connect ();

    private:

        //  Address to connect to.
        sockaddr_storage addr;
        socklen_t addr_len;

        //  Underlying socket.
        fd_t s;

        tcp_connecter_t (const tcp_connecter_t&);
        const tcp_connecter_t &operator = (const tcp_connecter_t&);
    };

}

#endif
