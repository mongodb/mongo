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

#include <string.h>

#include "../include/zmq.h"

#include "tcp_listener.hpp"
#include "platform.hpp"
#include "ip.hpp"
#include "config.hpp"
#include "err.hpp"

#ifdef ZMQ_HAVE_WINDOWS

zmq::tcp_listener_t::tcp_listener_t () :
    has_file (false),
    s (retired_fd)
{
    memset (&addr, 0, sizeof (addr));
    addr_len = 0;
}

zmq::tcp_listener_t::~tcp_listener_t ()
{
    if (s != retired_fd)
        close ();
}

int zmq::tcp_listener_t::set_address (const char *protocol_, const char *addr_,
    int backlog_)
{
    //  IPC protocol is not supported on Windows platform.
    if (strcmp (protocol_, "tcp") != 0 ) {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    //  Convert the interface into sockaddr_in structure.
    int rc = resolve_ip_interface (&addr, &addr_len, addr_);
    if (rc != 0)
        return rc;

    //  Create a listening socket.
    s = socket (addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        wsa_error_to_errno ();
        return -1;
    }

    //  Allow reusing of the address.
    int flag = 1;
    rc = setsockopt (s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        (const char*) &flag, sizeof (int));
    wsa_assert (rc != SOCKET_ERROR);

    //  Set the non-blocking flag.
    u_long uflag = 1;
    rc = ioctlsocket (s, FIONBIO, &uflag);
    wsa_assert (rc != SOCKET_ERROR);

    //  Bind the socket to the network interface and port.
    rc = bind (s, (struct sockaddr*) &addr, addr_len);
    if (rc == SOCKET_ERROR) {
        wsa_error_to_errno ();
        return -1;
    }

    //  Listen for incomming connections.
    rc = listen (s, backlog_);
    if (rc == SOCKET_ERROR) {
        wsa_error_to_errno ();
        return -1;
    }

    return 0;
}

int zmq::tcp_listener_t::close ()
{
    zmq_assert (s != retired_fd);
    int rc = closesocket (s);
    wsa_assert (rc != SOCKET_ERROR);
    s = retired_fd;
    return 0;
}

zmq::fd_t zmq::tcp_listener_t::get_fd ()
{
    return s;
}

zmq::fd_t zmq::tcp_listener_t::accept ()
{
    zmq_assert (s != retired_fd);

    //  Accept one incoming connection.
    fd_t sock = ::accept (s, NULL, NULL);
    if (sock == INVALID_SOCKET && 
          (WSAGetLastError () == WSAEWOULDBLOCK ||
          WSAGetLastError () == WSAECONNRESET))
        return retired_fd;

    zmq_assert (sock != INVALID_SOCKET);

    // Set to non-blocking mode.
    unsigned long argp = 1;
    int rc = ioctlsocket (sock, FIONBIO, &argp);
    wsa_assert (rc != SOCKET_ERROR);

    //  Disable Nagle's algorithm.
    int flag = 1;
    rc = setsockopt (sock, IPPROTO_TCP, TCP_NODELAY, (char*) &flag,
        sizeof (int));
    wsa_assert (rc != SOCKET_ERROR);

    return sock;
}

#else

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

#ifndef ZMQ_HAVE_OPENVMS
#include <sys/un.h>
#endif

#ifdef ZMQ_HAVE_OPENVMS
#include <ioctl.h>
#endif

zmq::tcp_listener_t::tcp_listener_t () :
    has_file (false),
    s (retired_fd)
{
    memset (&addr, 0, sizeof (addr));
}

zmq::tcp_listener_t::~tcp_listener_t ()
{
    if (s != retired_fd)
        close ();
}

int zmq::tcp_listener_t::set_address (const char *protocol_, const char *addr_,
    int backlog_)
{
    if (strcmp (protocol_, "tcp") == 0 ) {

        //  Resolve the sockaddr to bind to.
        int rc = resolve_ip_interface (&addr, &addr_len, addr_);
        if (rc != 0)
            return -1;

        //  Create a listening socket.
        s = open_socket (addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == -1)
            return -1;

        //  Allow reusing of the address.
        int flag = 1;
        rc = setsockopt (s, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof (int));
        errno_assert (rc == 0);

        //  Set the non-blocking flag.
#ifdef ZMQ_HAVE_OPENVMS
    	flag = 1;
    	rc = ioctl (s, FIONBIO, &flag);
        errno_assert (rc != -1);
#else
    	flag = fcntl (s, F_GETFL, 0);
    	if (flag == -1)
            flag = 0;
    	rc = fcntl (s, F_SETFL, flag | O_NONBLOCK);
        errno_assert (rc != -1);
#endif

        //  Bind the socket to the network interface and port.
        rc = bind (s, (struct sockaddr*) &addr, addr_len);
        if (rc != 0) {
            int err = errno;
            if (close () != 0)
                return -1;
            errno = err;
            return -1;
        }

        //  Listen for incomming connections.
        rc = listen (s, backlog_);
        if (rc != 0) {
            int err = errno;
            if (close () != 0)
                return -1;
            errno = err;
            return -1;
        }

        return 0;
    }
#ifndef ZMQ_HAVE_OPENVMS
    else if (strcmp (protocol_, "ipc") == 0) {

        //  Get rid of the file associated with the UNIX domain socket that
        //  may have been left behind by the previous run of the application.
        ::unlink (addr_);

        //  Convert the address into sockaddr_un structure.
        int rc = resolve_local_path (&addr, &addr_len, addr_);
        if (rc != 0)
            return -1;

        //  Create a listening socket.
        s = socket (AF_UNIX, SOCK_STREAM, 0);
        if (s == -1)
            return -1;

        //  Set the non-blocking flag.
        int flag = fcntl (s, F_GETFL, 0);
        if (flag == -1) 
            flag = 0;
        rc = fcntl (s, F_SETFL, flag | O_NONBLOCK);
        errno_assert (rc != -1);

        //  Bind the socket to the file path.
        rc = bind (s, (struct sockaddr*) &addr, addr_len);
        if (rc != 0) {
            int err = errno;
            if (close () != 0)
                return -1;
            errno = err;
            return -1;
        }
        has_file = true;

        //  Listen for incomming connections.
        rc = listen (s, backlog_);
        if (rc != 0) {
            int err = errno;
            if (close () != 0)
                return -1;
            errno = err;
            return -1;
        }

        return 0;
    }
#endif
    else {
        errno = EPROTONOSUPPORT;
        return -1;
    }    
}

int zmq::tcp_listener_t::close ()
{
    zmq_assert (s != retired_fd);
    int rc = ::close (s);
    if (rc != 0)
        return -1;
    s = retired_fd;

#ifndef ZMQ_HAVE_OPENVMS
    //  If there's an underlying UNIX domain socket, get rid of the file it
    //  is associated with.
    struct sockaddr_un *su = (struct sockaddr_un*) &addr;
    if (AF_UNIX == su->sun_family && has_file) {
        rc = ::unlink(su->sun_path);
        if (rc != 0)
            return -1;
    }
#endif

    return 0;
}

zmq::fd_t zmq::tcp_listener_t::get_fd ()
{
    return s;
}

zmq::fd_t zmq::tcp_listener_t::accept ()
{
    zmq_assert (s != retired_fd);

    //  Accept one incoming connection.
    fd_t sock = ::accept (s, NULL, NULL);

#if (defined ZMQ_HAVE_LINUX || defined ZMQ_HAVE_FREEBSD || \
     defined ZMQ_HAVE_OPENBSD || defined ZMQ_HAVE_OSX || \
     defined ZMQ_HAVE_OPENVMS || defined ZMQ_HAVE_NETBSD || \
     defined ZMQ_HAVE_CYGWIN)
    if (sock == -1 && 
        (errno == EAGAIN || errno == EWOULDBLOCK || 
         errno == EINTR || errno == ECONNABORTED))
        return retired_fd;
#elif (defined ZMQ_HAVE_SOLARIS || defined ZMQ_HAVE_AIX)
    if (sock == -1 && 
        (errno == EWOULDBLOCK || errno == EINTR || 
         errno == ECONNABORTED || errno == EPROTO))
        return retired_fd;
#elif defined ZMQ_HAVE_HPUX
    if (sock == -1 && 
        (errno == EAGAIN || errno == EWOULDBLOCK || 
         errno == EINTR || errno == ECONNABORTED || errno == ENOBUFS))
        return retired_fd;
#elif defined ZMQ_HAVE_QNXNTO 
    if (sock == -1 && 
        (errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED))
        return retired_fd;
#endif

    errno_assert (sock != -1); 

    // Set to non-blocking mode.
#ifdef ZMQ_HAVE_OPENVMS
    int flags = 1;
    int rc = ioctl (sock, FIONBIO, &flags);
    errno_assert (rc != -1);
#else
    int flags = fcntl (s, F_GETFL, 0);
    if (flags == -1)
        flags = 0;
    int rc = fcntl (sock, F_SETFL, flags | O_NONBLOCK);
    errno_assert (rc != -1);
#endif

    struct sockaddr *sa = (struct sockaddr*) &addr;
    if (AF_UNIX != sa->sa_family) {

        //  Disable Nagle's algorithm.
        int flag = 1;
        rc = setsockopt (sock, IPPROTO_TCP, TCP_NODELAY, (char*) &flag,
            sizeof (int));
        errno_assert (rc == 0);

#ifdef ZMQ_HAVE_OPENVMS
        //  Disable delayed acknowledgements.
        flag = 1;
        rc = setsockopt (sock, IPPROTO_TCP, TCP_NODELACK, (char*) &flag,
            sizeof (int));
        errno_assert (rc != SOCKET_ERROR);
#endif
    }

    return sock;
}

#endif
