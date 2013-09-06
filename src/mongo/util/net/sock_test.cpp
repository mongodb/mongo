/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/net/sock.h"

#include <boost/thread.hpp>

#ifndef _WIN32
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "mongo/db/cmdline.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

    CmdLine cmdLine;

    bool inShutdown() {
        return false;
    }

} // namespace mongo

namespace {

    using namespace mongo;

    typedef boost::shared_ptr<Socket> SocketPtr;
    typedef std::pair<SocketPtr, SocketPtr> SocketPair;

    // On UNIX, make a connected pair of PF_LOCAL (aka PF_UNIX) sockets via the native 'socketpair'
    // call. The 'type' parameter should be one of SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET, etc.
    // For Win32, we don't have a native socketpair function, so we hack up a connected PF_INET
    // pair on a random port.
    SocketPair socketPair(const int type, const int protocol = 0);

#if defined(_WIN32)
    namespace detail {
        void awaitAccept(SOCKET* acceptSock, SOCKET listenSock, Notification& notify) {
            *acceptSock = INVALID_SOCKET;
            const SOCKET result = ::accept(listenSock, NULL, 0);
            if (result != INVALID_SOCKET) {
                *acceptSock = result;
            }
            notify.notifyOne();
        }

        void awaitConnect(SOCKET* connectSock, const struct addrinfo& where, Notification& notify) {
            *connectSock = INVALID_SOCKET;
            SOCKET newSock = ::socket(where.ai_family, where.ai_socktype, where.ai_protocol);
            if (newSock != INVALID_SOCKET) {
                int result = ::connect(newSock, where.ai_addr, where.ai_addrlen);
                if (result == 0) {
                    *connectSock = newSock;
                }
            }
            notify.notifyOne();
        }
    } // namespace detail

    SocketPair socketPair(const int type, const int protocol) {

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

        int result = ::getaddrinfo(NULL, "0", &hints, &res);
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
        result = ::getaddrinfo(NULL, portStream.str().c_str(), &connectHints, &connectRes);
        if (result != 0) {
            closesocket(listenSock);
            ::freeaddrinfo(res);
            return SocketPair();
        }

        // I'd prefer to avoid trying to do this non-blocking on Windows. Just spin up some
        // threads to do the connect and acccept.

        Notification accepted;
        SOCKET acceptSock = INVALID_SOCKET;
        boost::thread acceptor(
            boost::bind(&detail::awaitAccept, &acceptSock, listenSock, boost::ref(accepted)));

        Notification connected;
        SOCKET connectSock = INVALID_SOCKET;
        boost::thread connector(
            boost::bind(&detail::awaitConnect, &connectSock, *connectRes, boost::ref(connected)));

        connected.waitToBeNotified();
        if (connectSock == INVALID_SOCKET) {
            closesocket(listenSock);
            ::freeaddrinfo(res);
            ::freeaddrinfo(connectRes);
            closesocket(acceptSock);
            closesocket(connectSock);
            return SocketPair();
        }

        accepted.waitToBeNotified();
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

        SocketPtr first(new Socket(static_cast<int>(acceptSock), SockAddr()));
        SocketPtr second(new Socket(static_cast<int>(connectSock), SockAddr()));

        return SocketPair(first, second);
    }
#else
    // We can just use ::socketpair and wrap up the result in a Socket.
    SocketPair socketPair(const int type, const int protocol) {
        // PF_LOCAL is the POSIX name for Unix domain sockets, while PF_UNIX
        // is the name that BSD used.  We use the BSD name because it is more
        // widely supported (e.g. Solaris 10).
        const int domain = PF_UNIX;

        int socks[2];
        const int result = ::socketpair(domain, type, protocol, socks);
        if (result == 0) {
            return SocketPair(
                SocketPtr(new Socket(socks[0], SockAddr())),
                SocketPtr(new Socket(socks[1], SockAddr())));
        }
        return SocketPair();
    }
#endif

    // This should match the name of the fail point declared in sock.cpp.
    const char kSocketFailPointName[] = "throwSockExcep";

    class SocketFailPointTest : public unittest::Test {
    public:

        SocketFailPointTest()
            : _failPoint(getGlobalFailPointRegistry()->getFailPoint(kSocketFailPointName))
            , _sockets(socketPair(SOCK_STREAM)) {
            ASSERT_TRUE(_failPoint != NULL);
            ASSERT_TRUE(_sockets.first);
            ASSERT_TRUE(_sockets.second);
        }

        ~SocketFailPointTest() {
        }

        bool trySend() {
            char byte = 'x';
            _sockets.first->send(&byte, sizeof(byte), "SocketFailPointTest::trySend");
            return true;
        }

        bool trySendVector() {
            std::vector<std::pair<char*, int> > data;
            char byte = 'x';
            data.push_back(std::make_pair(&byte, sizeof(byte)));
            _sockets.first->send(data, "SocketFailPointTest::trySendVector");
            return true;
        }

        bool tryRecv() {
            char byte;
            _sockets.second->recv(&byte, sizeof(byte));
            return true;
        }

        // You must queue at least one byte on the send socket before calling this function.
        size_t countRecvable(size_t max) {
            std::vector<char> buf(max);
            // This isn't great, because we don't have a guarantee that multiple sends will be
            // captured in one recv. However, sock doesn't let us pass flags into recv, so we
            // can't make this non blocking, and therefore can't risk another call.
            return _sockets.second->unsafe_recv(&buf[0], max);
        }

        FailPoint* const _failPoint;
        const SocketPair _sockets;
    };

    class ScopedFailPointEnabler {
    public:
        ScopedFailPointEnabler(FailPoint& fp)
            : _fp(fp) {
            _fp.setMode(FailPoint::alwaysOn);
        }

        ~ScopedFailPointEnabler() {
            _fp.setMode(FailPoint::off);
        }
    private:
        FailPoint& _fp;
    };

    TEST_F(SocketFailPointTest, TestSend) {
        ASSERT_TRUE(trySend());
        ASSERT_TRUE(tryRecv());
        {
            const ScopedFailPointEnabler enabled(*_failPoint);
            ASSERT_THROWS(trySend(), SocketException);
        }
        // Channel should be working again
        ASSERT_TRUE(trySend());
        ASSERT_TRUE(tryRecv());
    }

    TEST_F(SocketFailPointTest, TestSendVector) {
        ASSERT_TRUE(trySendVector());
        ASSERT_TRUE(tryRecv());
        {
            const ScopedFailPointEnabler enabled(*_failPoint);
            ASSERT_THROWS(trySendVector(), SocketException);
        }
        ASSERT_TRUE(trySendVector());
        ASSERT_TRUE(tryRecv());
    }

    TEST_F(SocketFailPointTest, TestRecv) {
        ASSERT_TRUE(trySend()); // data for recv
        ASSERT_TRUE(tryRecv());
        {
            ASSERT_TRUE(trySend()); // data for recv
            const ScopedFailPointEnabler enabled(*_failPoint);
            ASSERT_THROWS(tryRecv(), SocketException);
        }
        ASSERT_TRUE(trySend()); // data for recv
        ASSERT_TRUE(tryRecv());
    }

    TEST_F(SocketFailPointTest, TestFailedSendsDontSend) {
        ASSERT_TRUE(trySend());
        ASSERT_TRUE(tryRecv());
        {
            ASSERT_TRUE(trySend()); // queue 1 byte
            const ScopedFailPointEnabler enabled(*_failPoint);
            // Fail to queue another byte
            ASSERT_THROWS(trySend(), SocketException);
        }
        // Failed byte should not have been transmitted.
        ASSERT_EQUALS(size_t(1), countRecvable(2));
    }

    // Ensure that calling send doesn't actually enqueue data to the socket
    TEST_F(SocketFailPointTest, TestFailedVectorSendsDontSend) {
        ASSERT_TRUE(trySend());
        ASSERT_TRUE(tryRecv());
        {
            ASSERT_TRUE(trySend()); // queue 1 byte
            const ScopedFailPointEnabler enabled(*_failPoint);
            // Fail to queue another byte
            ASSERT_THROWS(trySendVector(), SocketException);
        }
        // Failed byte should not have been transmitted.
        ASSERT_EQUALS(size_t(1), countRecvable(2));
    }

    TEST_F(SocketFailPointTest, TestFailedRecvsDontRecv) {
        ASSERT_TRUE(trySend());
        ASSERT_TRUE(tryRecv());
        {
            ASSERT_TRUE(trySend());
            const ScopedFailPointEnabler enabled(*_failPoint);
            // Fail to recv that byte
            ASSERT_THROWS(tryRecv(), SocketException);
        }
        // Failed byte should still be queued to recv.
        ASSERT_EQUALS(size_t(1), countRecvable(1));
        // Channel should be working again
        ASSERT_TRUE(trySend());
        ASSERT_TRUE(tryRecv());
    }


} // namespace
