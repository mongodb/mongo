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

#include "../include/zmq.h"

#include "err.hpp"
#include "platform.hpp"

const char *zmq::errno_to_string (int errno_)
{
    switch (errno_) {
#if defined ZMQ_HAVE_WINDOWS
    case ENOTSUP:
        return "Not supported";
    case EPROTONOSUPPORT:
        return "Protocol not supported";
    case ENOBUFS:
        return "No buffer space available";
    case ENETDOWN:
        return "Network is down";
    case EADDRINUSE:
        return "Address in use";
    case EADDRNOTAVAIL:
        return "Address not available";
    case ECONNREFUSED:
        return "Connection refused";
    case EINPROGRESS:
        return "Operation in progress";
#endif
    case EFSM:
        return "Operation cannot be accomplished in current state";
    case ENOCOMPATPROTO:
        return "The protocol is not compatible with the socket type";
    case ETERM:
        return "Context was terminated";
    case EMTHREAD:
        return "No thread available";
    default:
#if defined _MSC_VER
#pragma warning (push)
#pragma warning (disable:4996)
#endif
        return strerror (errno_);
#if defined _MSC_VER
#pragma warning (pop)
#endif
    }
}

void zmq::zmq_abort(const char *errmsg_)
{
#if defined ZMQ_HAVE_WINDOWS

    //  Raise STATUS_FATAL_APP_EXIT.
    ULONG_PTR extra_info [1];
    extra_info [0] = (ULONG_PTR) errmsg_;
    RaiseException (0x40000015, EXCEPTION_NONCONTINUABLE, 1, extra_info);
#else
    abort ();
#endif
}

#ifdef ZMQ_HAVE_WINDOWS

const char *zmq::wsa_error()
{
    int no = WSAGetLastError ();
    //  TODO: This is not a generic way to handle this...
    if (no == WSAEWOULDBLOCK)
        return NULL;

    return wsa_error_no (no);
}

const char *zmq::wsa_error_no (int no_)
{
    //  TODO:  It seems that list of Windows socket errors is longer than this.
    //         Investigate whether there's a way to convert it into the string
    //         automatically (wsaError->HRESULT->string?).
    return
        (no_ == WSABASEERR) ?
            "No Error" : 
        (no_ == WSAEINTR) ?
            "Interrupted system call" : 
        (no_ == WSAEBADF) ?
            "Bad file number" : 
        (no_ == WSAEACCES) ?
            "Permission denied" : 
        (no_ == WSAEFAULT) ?
            "Bad address" : 
        (no_ == WSAEINVAL) ?
            "Invalid argument" : 
        (no_ == WSAEMFILE) ?
            "Too many open files" : 
        (no_ == WSAEWOULDBLOCK) ?
            "Operation would block" : 
        (no_ == WSAEINPROGRESS) ?
            "Operation now in progress" : 
        (no_ == WSAEALREADY) ?
            "Operation already in progress" : 
        (no_ == WSAENOTSOCK) ?
            "Socket operation on non-socket" : 
        (no_ == WSAEDESTADDRREQ) ?
            "Destination address required" : 
        (no_ == WSAEMSGSIZE) ?
            "Message too long" : 
        (no_ == WSAEPROTOTYPE) ?
            "Protocol wrong type for socket" : 
        (no_ == WSAENOPROTOOPT) ?
            "Bad protocol option" : 
        (no_ == WSAEPROTONOSUPPORT) ?
            "Protocol not supported" : 
        (no_ == WSAESOCKTNOSUPPORT) ?
            "Socket type not supported" : 
        (no_ == WSAEOPNOTSUPP) ?
            "Operation not supported on socket" : 
        (no_ == WSAEPFNOSUPPORT) ?
            "Protocol family not supported" : 
        (no_ == WSAEAFNOSUPPORT) ?
            "Address family not supported by protocol family" : 
        (no_ == WSAEADDRINUSE) ?
            "Address already in use" : 
        (no_ == WSAEADDRNOTAVAIL) ?
            "Can't assign requested address" : 
        (no_ == WSAENETDOWN) ?
            "Network is down" : 
        (no_ == WSAENETUNREACH) ?
            "Network is unreachable" : 
        (no_ == WSAENETRESET) ?
            "Net dropped connection or reset" : 
        (no_ == WSAECONNABORTED) ?
            "Software caused connection abort" : 
        (no_ == WSAECONNRESET) ?
            "Connection reset by peer" : 
        (no_ == WSAENOBUFS) ?
            "No buffer space available" : 
        (no_ == WSAEISCONN) ?
            "Socket is already connected" : 
        (no_ == WSAENOTCONN) ?
            "Socket is not connected" : 
        (no_ == WSAESHUTDOWN) ?
            "Can't send after socket shutdown" : 
        (no_ == WSAETOOMANYREFS) ?
            "Too many references can't splice" : 
        (no_ == WSAETIMEDOUT) ?
            "Connection timed out" : 
        (no_ == WSAECONNREFUSED) ?
            "Connection refused" : 
        (no_ == WSAELOOP) ?
            "Too many levels of symbolic links" : 
        (no_ == WSAENAMETOOLONG) ?
            "File name too long" : 
        (no_ == WSAEHOSTDOWN) ?
            "Host is down" : 
        (no_ == WSAEHOSTUNREACH) ?
            "No Route to Host" : 
        (no_ == WSAENOTEMPTY) ?
            "Directory not empty" : 
        (no_ == WSAEPROCLIM) ?
            "Too many processes" : 
        (no_ == WSAEUSERS) ?
            "Too many users" : 
        (no_ == WSAEDQUOT) ?
            "Disc Quota Exceeded" : 
        (no_ == WSAESTALE) ?
            "Stale NFS file handle" : 
        (no_ == WSAEREMOTE) ?
            "Too many levels of remote in path" : 
        (no_ == WSASYSNOTREADY) ?
            "Network SubSystem is unavailable" : 
        (no_ == WSAVERNOTSUPPORTED) ?
            "WINSOCK DLL Version out of range" : 
        (no_ == WSANOTINITIALISED) ?
            "Successful WSASTARTUP not yet performed" : 
        (no_ == WSAHOST_NOT_FOUND) ?
            "Host not found" : 
        (no_ == WSATRY_AGAIN) ?
            "Non-Authoritative Host not found" : 
        (no_ == WSANO_RECOVERY) ?
            "Non-Recoverable errors: FORMERR REFUSED NOTIMP" : 
        (no_ == WSANO_DATA) ?
            "Valid name no data record of requested" :
        "error not defined"; 
}

void zmq::win_error (char *buffer_, size_t buffer_size_)
{
    DWORD errcode = GetLastError ();
    DWORD rc = FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, NULL, errcode, MAKELANGID(LANG_NEUTRAL,
        SUBLANG_DEFAULT), buffer_, buffer_size_, NULL );
    zmq_assert (rc);
}

void zmq::wsa_error_to_errno ()
{
    int errcode = WSAGetLastError ();
    switch (errcode) {
    case WSAEINPROGRESS:
        errno = EAGAIN;
        return;
    case WSAEBADF:
        errno = EBADF;
        return;
    case WSAEINVAL:
        errno = EINVAL;
        return;
    case WSAEMFILE:
        errno = EMFILE;
        return;
    case WSAEFAULT:
        errno = EFAULT;
        return;
    case WSAEPROTONOSUPPORT:
        errno = EPROTONOSUPPORT;
        return;
    case WSAENOBUFS:
        errno = ENOBUFS;
        return;
    case WSAENETDOWN:
        errno = ENETDOWN;
        return;
    case WSAEADDRINUSE:
        errno = EADDRINUSE;
        return;
    case WSAEADDRNOTAVAIL:
        errno = EADDRNOTAVAIL;
        return;
    default:
        wsa_assert (false);
    }
}

#endif
