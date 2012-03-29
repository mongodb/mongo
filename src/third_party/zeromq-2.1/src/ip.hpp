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

#ifndef __ZMQ_IP_HPP_INCLUDED__
#define __ZMQ_IP_HPP_INCLUDED__

#include "platform.hpp"

#ifdef ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#endif

#if !defined ZMQ_HAVE_WINDOWS && !defined ZMQ_HAVE_OPENVMS
#include <sys/un.h>
#endif

//  Some platforms (notably Darwin/OSX and NetBSD) do not define all AI_
//  flags for getaddrinfo(). This can be worked around safely by defining
//  these to 0.
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 0
#endif
#ifndef AI_NUMERICSERV
#define AI_NUMERICSERV 0
#endif

namespace zmq
{
    //  Same as socket(2), but allows for transparent tweaking the options.
    int open_socket (int domain_, int type_, int protocol_);

    //  Resolves network interface name in <nic-name>:<port> format. Symbol "*"
    //  (asterisk) resolves to INADDR_ANY (all network interfaces).
    int resolve_ip_interface (sockaddr_storage *addr_, socklen_t *addr_len_,
        char const *interface_);

    //  This function resolves a string in <hostname>:<port-number> format.
    //  Hostname can be either the name of the host or its IP address.
    int resolve_ip_hostname (sockaddr_storage *addr_, socklen_t *addr_len_,
        const char *hostname_);

    // This function sets up address for UNIX domain transport.
    int resolve_local_path (sockaddr_storage *addr_, socklen_t *addr_len_,
        const char* pathname_);
}

#endif 
