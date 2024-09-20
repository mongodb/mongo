/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/net/sock.h"

#include "mongo/db/server_options.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/sock_test_utils.h"
#include "mongo/util/net/socket_exception.h"

namespace {

using namespace mongo;

// This should match the name of the fail point declared in sock.cpp.
const char kSocketFailPointName[] = "throwSockExcep";

class SocketFailPointTest : public unittest::Test {
public:
    SocketFailPointTest()
        : _failPoint(globalFailPointRegistry().find(kSocketFailPointName)),
          _sockets(socketPair(SOCK_STREAM)) {
        ASSERT_TRUE(_failPoint != nullptr);
        ASSERT_TRUE(_sockets.first);
        ASSERT_TRUE(_sockets.second);
    }

    ~SocketFailPointTest() override {}

    bool trySend() {
        char byte = 'x';
        _sockets.first->send(&byte, sizeof(byte), "SocketFailPointTest::trySend");
        return true;
    }

    bool trySendVector() {
        std::vector<std::pair<char*, int>> data;
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
    ScopedFailPointEnabler(FailPoint& fp) : _fp(fp) {
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
        auto expectedEx =
            makeSocketError(SocketErrorKind::SEND_ERROR, _sockets.first->remoteString());
        ASSERT_THROWS_WHAT(trySend(), NetworkException, expectedEx.reason());
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
        ASSERT_THROWS(trySendVector(), NetworkException);
    }
    ASSERT_TRUE(trySendVector());
    ASSERT_TRUE(tryRecv());
}

TEST_F(SocketFailPointTest, TestRecv) {
    ASSERT_TRUE(trySend());  // data for recv
    ASSERT_TRUE(tryRecv());
    {
        ASSERT_TRUE(trySend());  // data for recv
        const ScopedFailPointEnabler enabled(*_failPoint);
        auto expectedEx =
            makeSocketError(SocketErrorKind::RECV_ERROR, _sockets.first->remoteString());
        ASSERT_THROWS_WHAT(tryRecv(), NetworkException, expectedEx.reason());
    }
    ASSERT_TRUE(trySend());  // data for recv
    ASSERT_TRUE(tryRecv());
}

TEST_F(SocketFailPointTest, TestFailedSendsDontSend) {
    ASSERT_TRUE(trySend());
    ASSERT_TRUE(tryRecv());
    {
        ASSERT_TRUE(trySend());  // queue 1 byte
        const ScopedFailPointEnabler enabled(*_failPoint);
        // Fail to queue another byte
        ASSERT_THROWS(trySend(), NetworkException);
    }
    // Failed byte should not have been transmitted.
    ASSERT_EQUALS(size_t(1), countRecvable(2));
}

// Ensure that calling send doesn't actually enqueue data to the socket
TEST_F(SocketFailPointTest, TestFailedVectorSendsDontSend) {
    ASSERT_TRUE(trySend());
    ASSERT_TRUE(tryRecv());
    {
        ASSERT_TRUE(trySend());  // queue 1 byte
        const ScopedFailPointEnabler enabled(*_failPoint);
        // Fail to queue another byte
        ASSERT_THROWS(trySendVector(), NetworkException);
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
        ASSERT_THROWS(tryRecv(), NetworkException);
    }
    // Failed byte should still be queued to recv.
    ASSERT_EQUALS(size_t(1), countRecvable(1));
    // Channel should be working again
    ASSERT_TRUE(trySend());
    ASSERT_TRUE(tryRecv());
}


}  // namespace
