// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/net/sock_test_utils.h"

#ifndef _WIN32
#include <netdb.h>

#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/net/socket_exception.h"

namespace mongo {
namespace {

#if defined(_WIN32)
namespace detail {
void awaitAccept(SOCKET* acceptSock, SOCKET listenSock, Notification<void>& notify) {
    *acceptSock = INVALID_SOCKET;
    const SOCKET result = ::accept(listenSock, nullptr, 0);
    if (result != INVALID_SOCKET) {
        *acceptSock = result;
    }
    notify.set();
}

void awaitConnect(SOCKET* connectSock, const struct addrinfo& where, Notification<void>& notify) {
    *connectSock = INVALID_SOCKET;
    SOCKET newSock = ::socket(where.ai_family, where.ai_socktype, where.ai_protocol);
    if (newSock != INVALID_SOCKET) {
        int result = ::connect(newSock, where.ai_addr, where.ai_addrlen);
        if (result == 0) {
            *connectSock = newSock;
        }
    }
    notify.set();
}
}  // namespace detail

SocketPair socketPairImpl(const int type, const int protocol) {
    const int domain = PF_INET;

    // Create a listen socket and a connect socket.
    const SOCKET listenSock = ::socket(domain, type, protocol);
    if (listenSock == INVALID_SOCKET)
        return SocketPair();

    // Bind the listen socket on port zero, it will pick one for us, and start it listening
    // for connections.
    struct addrinfo hints, *res;
    ::memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_INET;
    hints.ai_socktype = type;
    hints.ai_flags = AI_PASSIVE;

    int result = ::getaddrinfo(nullptr, "0", &hints, &res);
    if (result != 0) {
        closesocket(listenSock);
        return SocketPair();
    }

    result = ::bind(listenSock, res->ai_addr, res->ai_addrlen);
    if (result != 0) {
        closesocket(listenSock);
        ::freeaddrinfo(res);
        return SocketPair();
    }

    // Read out the port to which we bound.
    sockaddr_in bindAddr;
    ::socklen_t len = sizeof(bindAddr);
    ::memset(&bindAddr, 0, sizeof(bindAddr));
    result = ::getsockname(listenSock, reinterpret_cast<struct sockaddr*>(&bindAddr), &len);
    if (result != 0) {
        closesocket(listenSock);
        ::freeaddrinfo(res);
        return SocketPair();
    }

    result = ::listen(listenSock, 1);
    if (result != 0) {
        closesocket(listenSock);
        ::freeaddrinfo(res);
        return SocketPair();
    }

    struct addrinfo connectHints, *connectRes;
    ::memset(&connectHints, 0, sizeof(connectHints));
    connectHints.ai_family = PF_INET;
    connectHints.ai_socktype = type;
    std::stringstream portStream;
    portStream << ntohs(bindAddr.sin_port);
    result = ::getaddrinfo(nullptr, portStream.str().c_str(), &connectHints, &connectRes);
    if (result != 0) {
        closesocket(listenSock);
        ::freeaddrinfo(res);
        return SocketPair();
    }

    // I'd prefer to avoid trying to do this non-blocking on Windows. Just spin up some
    // threads to do the connect and acccept.

    Notification<void> accepted;
    SOCKET acceptSock = INVALID_SOCKET;
    stdx::thread acceptor([&] { detail::awaitAccept(&acceptSock, listenSock, accepted); });

    Notification<void> connected;
    SOCKET connectSock = INVALID_SOCKET;
    stdx::thread connector([&] { detail::awaitConnect(&connectSock, *connectRes, connected); });

    connected.get();
    connector.join();
    if (connectSock == INVALID_SOCKET) {
        closesocket(listenSock);
        ::freeaddrinfo(res);
        ::freeaddrinfo(connectRes);
        closesocket(acceptSock);
        closesocket(connectSock);
        return SocketPair();
    }

    accepted.get();
    acceptor.join();
    if (acceptSock == INVALID_SOCKET) {
        closesocket(listenSock);
        ::freeaddrinfo(res);
        ::freeaddrinfo(connectRes);
        closesocket(acceptSock);
        closesocket(connectSock);
        return SocketPair();
    }

    closesocket(listenSock);
    ::freeaddrinfo(res);
    ::freeaddrinfo(connectRes);

    SocketPtr first = std::make_shared<Socket>(static_cast<int>(acceptSock), SockAddr());
    SocketPtr second = std::make_shared<Socket>(static_cast<int>(connectSock), SockAddr());
    return SocketPair(first, second);
}
#else
// We can just use ::socketpair and wrap up the result in a Socket.
SocketPair socketPairImpl(const int type, const int protocol) {
    // PF_LOCAL is the POSIX name for Unix domain sockets, while PF_UNIX
    // is the name that BSD used.  We use the BSD name because it is more
    // widely supported (e.g. Solaris 10).
    const int domain = PF_UNIX;

    int socks[2];
    const int result = ::socketpair(domain, type, protocol, socks);
    if (result == 0) {
        return SocketPair(std::make_shared<Socket>(socks[0], SockAddr()),
                          std::make_shared<Socket>(socks[1], SockAddr()));
    }
    return SocketPair();
}
#endif
}  // namespace

// On UNIX, make a connected pair of PF_LOCAL (aka PF_UNIX) sockets via the native 'socketpair'
// call. The 'type' parameter should be one of SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET, etc.
// For Win32, we don't have a native socketpair function, so we hack up a connected PF_INET
// pair on a random port.
SocketPair socketPair(int type, int protocol) {
    return socketPairImpl(type, protocol);
}

}  // namespace mongo
