/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <memory>

#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

namespace mongo::transport::grpc {
namespace {

class SessionTest : public ServiceContextWithClockSourceMockTest {
public:
    auto makeMessage() {
        OpMsg msg;
        msg.body = BSON("id" << _nextMessage.fetchAndAdd(1));
        return msg.serialize();
    }

private:
    AtomicWord<int> _nextMessage{0};
};

class IngressSessionTest : public SessionTest {
public:
    static constexpr auto kStreamTimeout = Seconds(1);
    static constexpr auto kClientId = "c08663ac-2f6c-408d-8829-97e67eef9f23";
    static constexpr auto kRemote = "FakeHost:1234";

    void setUp() override {
        SessionTest::setUp();
        _fixture = std::make_unique<MockStreamTestFixtures>(
            HostAndPort{kRemote}, kStreamTimeout, _clientMetadata);
    }

    void tearDown() override {
        _fixture.reset();
    }

    auto fixture() {
        return _fixture.get();
    }

    auto makeSession() {
        auto swClientId = UUID::parse(kClientId);
        ASSERT_OK(swClientId.getStatus());
        return std::make_unique<IngressSession>(nullptr,
                                                _fixture->serverCtx.get(),
                                                _fixture->serverStream.get(),
                                                swClientId.getValue());
    }

private:
    const MetadataView _clientMetadata = {{"foo", "bar"}};
    std::unique_ptr<MockStreamTestFixtures> _fixture;
};

TEST_F(IngressSessionTest, NoClientId) {
    auto session = std::make_unique<IngressSession>(
        nullptr, fixture()->serverCtx.get(), fixture()->serverStream.get(), boost::none);
    ASSERT_FALSE(session->clientId());
    session->end();
}

TEST_F(IngressSessionTest, AlwaysCancelsStream) {
    auto session = makeSession();
    ASSERT_FALSE(fixture()->serverCtx->isCancelled());
    session.reset();
    ASSERT_TRUE(fixture()->serverCtx->isCancelled());
}

TEST_F(IngressSessionTest, GetClientId) {
    auto session = makeSession();
    ASSERT_TRUE(session->clientId());
    ASSERT_EQ(session->clientId()->toString(), kClientId);
}

TEST_F(IngressSessionTest, GetRemote) {
    auto session = makeSession();
    ASSERT_EQ(session->remote().toString(), kRemote);
}

TEST_F(IngressSessionTest, IsConnected) {
    auto session = makeSession();
    ASSERT_TRUE(session->isConnected());
    fixture()->serverCtx->tryCancel();
    ASSERT_FALSE(session->isConnected());
}

TEST_F(IngressSessionTest, Terminate) {
    auto session = makeSession();
    session->terminate(Status::OK());
    ASSERT_FALSE(session->isConnected());
    ASSERT_TRUE(session->terminationStatus());
    ASSERT_OK(*session->terminationStatus());
}

TEST_F(IngressSessionTest, TerminateWithError) {
    const Status error(ErrorCodes::InternalError, "Some Error");
    auto session = makeSession();
    session->terminate(error);
    ASSERT_FALSE(session->isConnected());
    ASSERT_TRUE(session->terminationStatus());
    ASSERT_EQ(*session->terminationStatus(), error);
}

TEST_F(IngressSessionTest, TerminateRetainsStatus) {
    const Status error(ErrorCodes::InternalError, "Some Error");
    auto session = makeSession();
    session->terminate(error);
    session->terminate(Status::OK());
    ASSERT_EQ(*session->terminationStatus(), error);
}

TEST_F(IngressSessionTest, ReadAndWrite) {
    auto session = makeSession();
    {
        auto msg = makeMessage();
        fixture()->clientStream->write(msg.sharedBuffer());
        auto swReceived = session->sourceMessage();
        ASSERT_OK(swReceived.getStatus());
        ASSERT_EQ_MSG(swReceived.getValue(), msg);
    }

    {
        auto msg = makeMessage();
        ASSERT_OK(session->sinkMessage(msg));
        auto sent = fixture()->clientStream->read();
        ASSERT_TRUE(sent);
        ASSERT_EQ_MSG(Message{*sent}, msg);
    }
}

TEST_F(IngressSessionTest, ReadFromClosedStream) {
    auto session = makeSession();
    fixture()->serverCtx->tryCancel();
    auto swReceived = session->sourceMessage();
    ASSERT_EQ(swReceived.getStatus(), ErrorCodes::StreamTerminated);
}

TEST_F(IngressSessionTest, ReadTimesOut) {
    auto session = makeSession();
    clockSource().advance(2 * kStreamTimeout);
    auto swReceived = session->sourceMessage();
    ASSERT_EQ(swReceived.getStatus(), ErrorCodes::StreamTerminated);
}

TEST_F(IngressSessionTest, WriteToClosedStream) {
    auto session = makeSession();
    fixture()->serverCtx->tryCancel();
    ASSERT_EQ(session->sinkMessage(Message{}), ErrorCodes::StreamTerminated);
}

TEST_F(IngressSessionTest, WriteTimesOut) {
    auto session = makeSession();
    clockSource().advance(2 * kStreamTimeout);
    ASSERT_EQ(session->sinkMessage(Message{}), ErrorCodes::StreamTerminated);
}

}  // namespace
}  // namespace mongo::transport::grpc
