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

#include "tcp_socket.hpp"
#include "platform.hpp"
#include "err.hpp"

#ifdef ZMQ_HAVE_WINDOWS

zmq::tcp_socket_t::tcp_socket_t () :
    s (retired_fd)
{
}

zmq::tcp_socket_t::~tcp_socket_t ()
{
    if (s != retired_fd)
        close ();
}

int zmq::tcp_socket_t::open (fd_t fd_, uint64_t sndbuf_, uint64_t rcvbuf_)
{
    zmq_assert (s == retired_fd);
    s = fd_;

    if (sndbuf_) {
        int sz = (int) sndbuf_;
        int rc = setsockopt (s, SOL_SOCKET, SO_SNDBUF,
            (char*) &sz, sizeof (int));
        errno_assert (rc == 0);
    }

    if (rcvbuf_) {
        int sz = (int) rcvbuf_;
        int rc = setsockopt (s, SOL_SOCKET, SO_RCVBUF,
            (char*) &sz, sizeof (int));
        errno_assert (rc == 0);
    }

    return 0;
}

int zmq::tcp_socket_t::close ()
{
    zmq_assert (s != retired_fd);
    int rc = closesocket (s);
    wsa_assert (rc != SOCKET_ERROR);
    s = retired_fd;
    return 0;
}

zmq::fd_t zmq::tcp_socket_t::get_fd ()
{
    return s;
}

int zmq::tcp_socket_t::write (const void *data, int size)
{
    int nbytes = send (s, (char*) data, size, 0);

    //  If not a single byte can be written to the socket in non-blocking mode
    //  we'll get an error (this may happen during the speculative write).
    if (nbytes == SOCKET_ERROR && WSAGetLastError () == WSAEWOULDBLOCK)
        return 0;
		
    //  Signalise peer failure.
    if (nbytes == -1 && (
          WSAGetLastError () == WSAENETDOWN ||
          WSAGetLastError () == WSAENETRESET ||
          WSAGetLastError () == WSAEHOSTUNREACH ||
          WSAGetLastError () == WSAECONNABORTED ||
          WSAGetLastError () == WSAETIMEDOUT ||
          WSAGetLastError () == WSAECONNRESET))
        return -1;

    wsa_assert (nbytes != SOCKET_ERROR);

    return (size_t) nbytes;
}

int zmq::tcp_socket_t::read (void *data, int size)
{
    int nbytes = recv (s, (char*) data, size, 0);

    //  If not a single byte can be read from the socket in non-blocking mode
    //  we'll get an error (this may happen during the speculative read).
    if (nbytes == SOCKET_ERROR && WSAGetLastError () == WSAEWOULDBLOCK)
        return 0;

    //  Connection failure.
    if (nbytes == -1 && (
          WSAGetLastError () == WSAENETDOWN ||
          WSAGetLastError () == WSAENETRESET ||
          WSAGetLastError () == WSAECONNABORTED ||
          WSAGetLastError () == WSAETIMEDOUT ||
          WSAGetLastError () == WSAECONNRESET ||
          WSAGetLastError () == WSAECONNREFUSED ||
          WSAGetLastError () == WSAENOTCONN))
        return -1;

    wsa_assert (nbytes != SOCKET_ERROR);

    //  Orderly shutdown by the other peer.
    if (nbytes == 0)
        return -1; 

    return (size_t) nbytes;
}

#else

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

zmq::tcp_socket_t::tcp_socket_t () :
    s (retired_fd)
{
}

zmq::tcp_socket_t::~tcp_socket_t ()
{
    if (s != retired_fd)
        close ();
}

int zmq::tcp_socket_t::open (fd_t fd_, uint64_t sndbuf_, uint64_t rcvbuf_)
{
    assert (s == retired_fd);
    s = fd_;

    if (sndbuf_) {
        int sz = (int) sndbuf_;
        int rc = setsockopt (s, SOL_SOCKET, SO_SNDBUF, &sz, sizeof (int));
        errno_assert (rc == 0);
    }

    if (rcvbuf_) {
        int sz = (int) rcvbuf_;
        int rc = setsockopt (s, SOL_SOCKET, SO_RCVBUF, &sz, sizeof (int));
        errno_assert (rc == 0);
    }

#if defined ZMQ_HAVE_OSX || defined ZMQ_HAVE_FREEBSD
    int set = 1;
    int rc = setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof (int));
    errno_assert (rc == 0);
#endif
    return 0;
}

int zmq::tcp_socket_t::close ()
{
    zmq_assert (s != retired_fd);
    int rc = ::close (s);
    if (rc != 0)
        return -1;
    s = retired_fd;
    return 0;
}

zmq::fd_t zmq::tcp_socket_t::get_fd ()
{
    return s;
}

int zmq::tcp_socket_t::write (const void *data, int size)
{
    ssize_t nbytes = send (s, data, size, 0);

    //  Several errors are OK. When speculative write is being done we may not
    //  be able to write a single byte to the socket. Also, SIGSTOP issued
    //  by a debugging tool can result in EINTR error.
    if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == EINTR))
        return 0;

    //  Signalise peer failure.
    if (nbytes == -1 && (errno == ECONNRESET || errno == EPIPE))
        return -1;

    errno_assert (nbytes != -1);
    return (size_t) nbytes;
}

int zmq::tcp_socket_t::read (void *data, int size)
{
    ssize_t nbytes = recv (s, data, size, 0);

    //  Several errors are OK. When speculative read is being done we may not
    //  be able to read a single byte to the socket. Also, SIGSTOP issued
    //  by a debugging tool can result in EINTR error.
    if (nbytes == -1
    && (errno == EAGAIN
     || errno == EWOULDBLOCK
     || errno == EINTR))
        return 0;

    //  Signal peer failure.
    if (nbytes == -1
    && (errno == ECONNRESET
     || errno == ECONNREFUSED
     || errno == ETIMEDOUT
     || errno == EHOSTUNREACH
     || errno == ENOTCONN))
        return -1;

    errno_assert (nbytes != -1);

    //  Orderly shutdown by the other peer.
    if (nbytes == 0)
        return -1;

    return (size_t) nbytes;
}

#endif

