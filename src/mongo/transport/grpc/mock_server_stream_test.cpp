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

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/mock_server_context.h"
#include "mongo/transport/grpc/mock_server_stream.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/uuid.h"

namespace mongo::transport::grpc {

template <class Base>
class MockServerStreamBase : public Base {
public:
    static constexpr Milliseconds kTimeout = Milliseconds(100);
    static constexpr const char* kRemote = "abc:123";

    virtual void setUp() override {
        Base::setUp();
        _fixtures = std::make_unique<MockStreamTestFixtures>(
            HostAndPort{kRemote}, kTimeout, _clientMetadata);
    }

    MockStreamTestFixtures& getFixtures() {
        return *_fixtures;
    }

    MockServerStream& getServerStream() {
        return *getFixtures().serverStream;
    }

    MockServerContext& getServerContext() {
        return *getFixtures().serverCtx;
    }

    MockClientStream& getClientStream() {
        return *getFixtures().clientStream;
    }

    MockClientContext& getClientContext() {
        return *getFixtures().clientCtx;
    }

    const Message& getClientFirstMessage() const {
        return _clientFirstMessage;
    }

    const MetadataView& getClientMetadata() const {
        return _clientMetadata;
    }

    /**
     * Verifies that MockServerContext::tryCancel() interrupts the blocking operation performed by
     * the provided closure.
     */
    template <typename F>
    void tryCancelTest(F f) {
        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            Client::initThread("tryCancelTest");

            auto opCtx = Base::makeOperationContext();
            Notification<void> opDone;
            Notification<void> setupDone;

            auto opThread = monitor.spawn([&opDone, &setupDone, &f]() {
                ON_BLOCK_EXIT([&] { opDone.set(); });
                f(setupDone);
            });

            ASSERT_TRUE(setupDone.waitFor(opCtx.get(), Milliseconds(100)))
                << "operation thread should start executing in a timely manner";

            ASSERT_FALSE(opDone.waitFor(opCtx.get(), Milliseconds(25)))
                << "operation thread should not finish even after waiting for a long time";

            getServerContext().tryCancel();

            ASSERT_TRUE(opDone.waitFor(opCtx.get(), Milliseconds(25)))
                << "operation thread should be finished after cancel";

            opThread.join();
        });
    }

private:
    const MetadataView _clientMetadata = {{"foo", "bar"}, {"baz", "zoo"}};
    const Message _clientFirstMessage = makeUniqueMessage();
    std::unique_ptr<MockStreamTestFixtures> _fixtures;
};

class MockServerStreamTest : public MockServerStreamBase<LockerNoopServiceContextTest> {};

class MockServerStreamTestWithMockedClockSource
    : public MockServerStreamBase<ServiceContextWithClockSourceMockTest> {};

TEST_F(MockServerStreamTest, SendReceiveMessage) {
    auto clientFirst = makeUniqueMessage();
    ASSERT_TRUE(getClientStream().write(clientFirst.sharedBuffer()));
    auto readMsg = getServerStream().read();
    ASSERT_TRUE(readMsg);
    ASSERT_EQ_MSG(Message{*readMsg}, clientFirst);

    auto serverResponse = makeUniqueMessage();
    ASSERT_TRUE(getServerStream().write(serverResponse.sharedBuffer()));

    auto clientReceivedMsg = getClientStream().read();
    ASSERT_TRUE(clientReceivedMsg);
    ASSERT_EQ_MSG(Message{*clientReceivedMsg}, serverResponse);
}

TEST_F(MockServerStreamTest, ConcurrentAccessToStream) {
    unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
        auto message = makeUniqueMessage();

        auto readThread = monitor.spawn([&]() {
            auto readMsg = getServerStream().read();
            ASSERT_TRUE(readMsg);
            ASSERT_EQ_MSG(Message{*readMsg}, message);
        });

        auto writeThread =
            monitor.spawn([&]() { ASSERT_TRUE(getServerStream().write(message.sharedBuffer())); });

        ASSERT_TRUE(getClientStream().write(message.sharedBuffer()));
        auto clientReceived = getClientStream().read();
        ASSERT_TRUE(clientReceived);
        ASSERT_EQ_MSG(Message{*clientReceived}, Message{message});

        writeThread.join();
        readThread.join();
    });
}

TEST_F(MockServerStreamTest, SendReceiveEmptyInitialMetadata) {
    ASSERT_TRUE(getServerStream().write(makeUniqueMessage().sharedBuffer()));
    ASSERT_TRUE(getClientStream().read());
    ASSERT_TRUE(getClientContext().getServerInitialMetadata());
    ASSERT_EQ(getClientContext().getServerInitialMetadata()->size(), 0);
}

TEST_F(MockServerStreamTest, SendReceiveInitialMetadata) {
    MetadataContainer expected = {
        {"foo", "bar"}, {"baz", "more metadata"}, {"baz", "repeated key"}};

    for (auto& kvp : expected) {
        getServerContext().addInitialMetadataEntry(kvp.first, kvp.second);
    }
    ASSERT_TRUE(getServerStream().write(makeUniqueMessage().sharedBuffer()));

    ASSERT_TRUE(getClientStream().read());
    ASSERT_TRUE(getClientContext().getServerInitialMetadata());
    ASSERT_EQ(*getClientContext().getServerInitialMetadata(), expected);
}

DEATH_TEST_F(MockServerStreamTest, CannotModifyMetadataAfterSent, "invariant") {
    getServerContext().addInitialMetadataEntry("foo", "bar");
    auto serverResponse = makeUniqueMessage();
    ASSERT_TRUE(getServerStream().write(serverResponse.sharedBuffer()));

    getServerContext().addInitialMetadataEntry("cant", "add metadata after it has been sent");
}

TEST_F(MockServerStreamTest, CannotRetrieveMetadataBeforeSent) {
    getServerContext().addInitialMetadataEntry("foo", "bar");

    ASSERT_FALSE(getClientContext().getServerInitialMetadata());
    ASSERT_TRUE(getServerStream().write(makeUniqueMessage().sharedBuffer()));
    ASSERT_TRUE(getClientContext().getServerInitialMetadata());
}

TEST_F(MockServerStreamTestWithMockedClockSource, DeadlineIsEnforced) {
    clockSource().advance(kTimeout * 2);
    ASSERT_TRUE(getServerContext().isCancelled());
    ASSERT_FALSE(getServerStream().read());
    ASSERT_FALSE(getServerStream().write(makeUniqueMessage().sharedBuffer()));
    ASSERT_FALSE(getClientContext().getServerInitialMetadata());
    ASSERT_FALSE(getClientStream().read());
}

TEST_F(MockServerStreamTest, TryCancelSubsequentServerRead) {
    tryCancelTest([&](Notification<void>& setupDone) {
        auto msg = makeUniqueMessage();
        ASSERT_TRUE(getServerStream().write(msg.sharedBuffer()));
        setupDone.set();
        ASSERT_FALSE(getServerStream().read());
    });
}

TEST_F(MockServerStreamTest, TryCancelInitialClientRead) {
    tryCancelTest([&](Notification<void>& setupDone) {
        setupDone.set();
        ASSERT_FALSE(getClientStream().read());
    });
}

TEST_F(MockServerStreamTest, TryCancelWrite) {
    getServerContext().tryCancel();
    auto msg = makeUniqueMessage();
    ASSERT_FALSE(getServerStream().write(msg.sharedBuffer()));
    ASSERT_FALSE(getClientStream().write(msg.sharedBuffer()));
    ASSERT_FALSE(getClientContext().getServerInitialMetadata());
}

TEST_F(MockServerStreamTest, MetadataAvailableAfterTryCancel) {
    auto msg = makeUniqueMessage();
    ASSERT_TRUE(getServerStream().write(msg.sharedBuffer()));
    getServerContext().tryCancel();
    ASSERT_TRUE(getClientContext().getServerInitialMetadata());
}

TEST_F(MockServerStreamTest, InitialServerReadTimesOut) {
    ASSERT_FALSE(getServerStream().read());
}

TEST_F(MockServerStreamTest, InitialClientReadTimesOut) {
    ASSERT_FALSE(getClientStream().read());
}

TEST_F(MockServerStreamTest, ClientMetadataIsAccessible) {
    ASSERT_EQ(getServerContext().getClientMetadata(), getClientMetadata());
}

}  // namespace mongo::transport::grpc
