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

#include <string>

#include "../include/zmq.h"

#include "tcp_connecter.hpp"
#include "platform.hpp"
#include "ip.hpp"
#include "err.hpp"

#ifdef ZMQ_HAVE_WINDOWS

zmq::tcp_connecter_t::tcp_connecter_t () :
    s (retired_fd)
{
    memset (&addr, 0, sizeof (addr));
    addr_len = 0;
}

zmq::tcp_connecter_t::~tcp_connecter_t ()
{
    if (s != retired_fd)
        close ();
}

int zmq::tcp_connecter_t::set_address (const char *protocol_, const char *addr_)
{
    if (strcmp (protocol_, "tcp") == 0)
        return resolve_ip_hostname (&addr, &addr_len, addr_);

    errno = EPROTONOSUPPORT;
    return -1;
}

int zmq::tcp_connecter_t::open ()
{
    zmq_assert (s == retired_fd);

    //  Create the socket.
    s = open_socket (addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        wsa_error_to_errno ();
        return -1;
    }

    // Set to non-blocking mode.
    unsigned long argp = 1;
    int rc = ioctlsocket (s, FIONBIO, &argp);
    wsa_assert (rc != SOCKET_ERROR);

    //  Disable Nagle's algorithm.
    int flag = 1;
    rc = setsockopt (s, IPPROTO_TCP, TCP_NODELAY, (char*) &flag,
        sizeof (int));
    wsa_assert (rc != SOCKET_ERROR);

    //  Connect to the remote peer.
    rc = ::connect (s, (sockaddr*) &addr, addr_len);

    //  Connect was successfull immediately.
    if (rc == 0)
        return 0;

    //  Asynchronous connect was launched.
    if (rc == SOCKET_ERROR && (WSAGetLastError () == WSAEINPROGRESS ||
          WSAGetLastError () == WSAEWOULDBLOCK)) {
        errno = EAGAIN;
        return -1;
    }

    wsa_error_to_errno ();
    return -1;
}

int zmq::tcp_connecter_t::close ()
{
    zmq_assert (s != retired_fd);
    int rc = closesocket (s);
    wsa_assert (rc != SOCKET_ERROR);
    s = retired_fd;
    return 0;
}

zmq::fd_t zmq::tcp_connecter_t::get_fd ()
{
    return s;
}

zmq::fd_t zmq::tcp_connecter_t::connect ()
{
    //  Nonblocking connect have finished. Check whether an error occured.
    int err = 0;
    socklen_t len = sizeof err;
    int rc = getsockopt (s, SOL_SOCKET, SO_ERROR, (char*) &err, &len);
    zmq_assert (rc == 0);
    if (err != 0) {

        //  Assert that the error was caused by the networking problems
        //  rather than 0MQ bug.
        if (err == WSAECONNREFUSED || err == WSAETIMEDOUT ||
              err == WSAECONNABORTED || err == WSAEHOSTUNREACH ||
              err == WSAENETUNREACH || err == WSAENETDOWN)
            return retired_fd;

        wsa_assert_no (err);
    }

    //  Return the newly connected socket.
    fd_t result = s;
    s = retired_fd;
    return result;
}

#else

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

#ifdef ZMQ_HAVE_OPENVMS
#include <ioctl.h>
#endif

zmq::tcp_connecter_t::tcp_connecter_t () :
    s (retired_fd)
{
    memset (&addr, 0, sizeof (addr));
}

zmq::tcp_connecter_t::~tcp_connecter_t ()
{
    if (s != retired_fd)
        close ();
}

int zmq::tcp_connecter_t::set_address (const char *protocol_, const char *addr_)
{
    if (strcmp (protocol_, "tcp") == 0)
        return resolve_ip_hostname (&addr, &addr_len, addr_);
    else
    if (strcmp (protocol_, "ipc") == 0)
        return resolve_local_path (&addr, &addr_len, addr_);

    errno = EPROTONOSUPPORT;
    return -1;
}

int zmq::tcp_connecter_t::open ()
{
    zmq_assert (s == retired_fd);
    struct sockaddr *sa = (struct sockaddr*) &addr;

    if (AF_UNIX != sa->sa_family) {

        //  Create the socket.
        s = open_socket (sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == -1)
            return -1;

        // Set to non-blocking mode.
#ifdef ZMQ_HAVE_OPENVMS
    	int flags = 1;
    	int rc = ioctl (s, FIONBIO, &flags);
        errno_assert (rc != -1);
#else
    	int flags = fcntl (s, F_GETFL, 0);
    	if (flags == -1)
            flags = 0;
    	int rc = fcntl (s, F_SETFL, flags | O_NONBLOCK);
        errno_assert (rc != -1);
#endif

        //  Disable Nagle's algorithm.
        int flag = 1;
        rc = setsockopt (s, IPPROTO_TCP, TCP_NODELAY, (char*) &flag,
            sizeof (int));
        errno_assert (rc == 0);

#ifdef ZMQ_HAVE_OPENVMS
        //  Disable delayed acknowledgements.
        flag = 1;
        rc = setsockopt (s, IPPROTO_TCP, TCP_NODELACK, (char*) &flag,
            sizeof (int));
        errno_assert (rc != SOCKET_ERROR);
#endif

        //  Connect to the remote peer.
        rc = ::connect (s, (struct sockaddr*) &addr, addr_len);

        //  Connect was successfull immediately.
        if (rc == 0)
            return 0;

        //  Asynchronous connect was launched.
        if (rc == -1 && errno == EINPROGRESS) {
            errno = EAGAIN;
            return -1;
        }

        //  Error occured.
        int err = errno;
        close ();
        errno = err;
        return -1;
    }

#ifndef ZMQ_HAVE_OPENVMS
    else {

        //  Create the socket.
        zmq_assert (AF_UNIX == sa->sa_family);
        s = open_socket (AF_UNIX, SOCK_STREAM, 0);
        if (s == -1)
            return -1;

        //  Set the non-blocking flag.
        int flag = fcntl (s, F_GETFL, 0);
        if (flag == -1)
            flag = 0;
        int rc = fcntl (s, F_SETFL, flag | O_NONBLOCK);
        errno_assert (rc != -1);

        //  Connect to the remote peer.
        rc = ::connect (s, (struct sockaddr*) &addr, sizeof (sockaddr_un));

        //  Connect was successfull immediately.
        if (rc == 0)
            return 0;

        //  Error occured.
        int err = errno;
        close ();
        errno = err;
        return -1;
    }
#endif

    zmq_assert (false);
    return -1;
}

int zmq::tcp_connecter_t::close ()
{
    zmq_assert (s != retired_fd);
    int rc = ::close (s);
    if (rc != 0)
        return -1;
    s = retired_fd;
    return 0;
}

zmq::fd_t zmq::tcp_connecter_t::get_fd ()
{
    return s;
}

zmq::fd_t zmq::tcp_connecter_t::connect ()
{
    //  Following code should handle both Berkeley-derived socket
    //  implementations and Solaris.
    int err = 0;
#if defined ZMQ_HAVE_HPUX
    int len = sizeof (err);
#else
    socklen_t len = sizeof (err);
#endif
    int rc = getsockopt (s, SOL_SOCKET, SO_ERROR, (char*) &err, &len);
    if (rc == -1)
        err = errno;
    if (err != 0) {

        //  Assert if the error was caused by 0MQ bug.
        //  Networking problems are OK. No need to assert.
        errno = err;
        errno_assert (errno == ECONNREFUSED || errno == ECONNRESET ||
            errno == ETIMEDOUT || errno == EHOSTUNREACH ||
            errno == ENETUNREACH);

        return retired_fd;
    }

    fd_t result = s;
    s = retired_fd;
    return result;
}

#endif
