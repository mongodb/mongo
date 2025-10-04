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

#include "mongo/transport/grpc/mock_server_stream.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/mock_server_context.h"
#include "mongo/transport/grpc/mock_stub.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/uuid.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/support/status.h>

namespace mongo::transport::grpc {

template <class Base>
class MockServerStreamBase : public Base {
public:
    void setUp() override {
        Base::setUp();

        MockStubTestFixtures fixtures;
        _reactor = std::make_shared<GRPCReactor>();
        _deadline = Base::getServiceContext()->getFastClockSource()->now() + getTimeout();
        _fixtures = fixtures.makeStreamTestFixtures(_deadline, _clientMetadata, _reactor);
    }

    virtual Milliseconds getTimeout() const {
        return Milliseconds(5000);
    }

    MockServerStream& getServerStream() {
        return *_fixtures->rpc->serverStream;
    }

    MockServerContext& getServerContext() {
        return *_fixtures->rpc->serverCtx;
    }

    MockClientStream& getClientStream() {
        return *_fixtures->clientStream;
    }

    ClientContext& getClientContext() {
        return *_fixtures->clientCtx;
    }

    const Message& getClientFirstMessage() const {
        return _clientFirstMessage;
    }

    const MetadataView& getClientMetadata() const {
        return _clientMetadata;
    }

    const std::shared_ptr<GRPCReactor>& getReactor() {
        return _reactor;
    }

    /**
     * Verifies that MockServerContext::tryCancel() interrupts the blocking operation performed by
     * the provided closure.
     */
    template <typename F>
    void tryCancelTest(F f) {
        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            mongo::Client::initThread("tryCancelTest", getGlobalServiceContext()->getService());

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

            // Operation thread should finish after cancel.
            opDone.get(opCtx.get());

            // Ensure that the operation finished via cancellation rather than the timeout.
            ASSERT_LT(Base::getServiceContext()->getFastClockSource()->now(), _deadline);

            opThread.join();
        });
    }

private:
    const MetadataView _clientMetadata = {{"foo", "bar"}, {"baz", "zoo"}};
    const Message _clientFirstMessage = makeUniqueMessage();
    std::unique_ptr<MockStreamTestFixtures> _fixtures;
    std::shared_ptr<GRPCReactor> _reactor;
    Date_t _deadline;
};

class MockServerStreamTest : public MockServerStreamBase<ServiceContextTest> {};

class MockServerStreamTestShortTimeout : public MockServerStreamTest {
public:
    Milliseconds getTimeout() const override {
        return Milliseconds(100);
    }
};

class MockServerStreamTestWithMockedClockSource
    : public MockServerStreamBase<ServiceContextWithClockSourceMockTest> {};

TEST_F(MockServerStreamTest, SendReceiveMessage) {
    auto clientFirst = makeUniqueMessage();
    ASSERT_TRUE(getClientStream().syncWrite(getReactor(), clientFirst.sharedBuffer()));
    auto readMsg = getServerStream().read();
    ASSERT_TRUE(readMsg);
    ASSERT_EQ_MSG(Message{*readMsg}, clientFirst);

    auto serverResponse = makeUniqueMessage();
    ASSERT_TRUE(getServerStream().write(serverResponse.sharedBuffer()));

    auto clientReceivedMsg = getClientStream().syncRead(getReactor());
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

        ASSERT_TRUE(getClientStream().syncWrite(getReactor(), message.sharedBuffer()));
        auto clientReceived = getClientStream().syncRead(getReactor());
        ASSERT_TRUE(clientReceived);
        ASSERT_EQ_MSG(Message{*clientReceived}, Message{message});

        writeThread.join();
        readThread.join();
    });
}

TEST_F(MockServerStreamTest, SendReceiveEmptyInitialMetadata) {
    ASSERT_TRUE(getServerStream().write(makeUniqueMessage().sharedBuffer()));
    ASSERT_TRUE(getClientStream().syncRead(getReactor()));
    ASSERT_EQ(getClientContext().getServerInitialMetadata().size(), 0);
}

TEST_F(MockServerStreamTest, SendReceiveInitialMetadata) {
    MetadataView expected = {{"foo", "bar"}, {"baz", "more metadata"}, {"baz", "repeated key"}};

    for (auto& kvp : expected) {
        getServerContext().addInitialMetadataEntry(std::string{kvp.first}, std::string{kvp.second});
    }
    ASSERT_TRUE(getServerStream().write(makeUniqueMessage().sharedBuffer()));

    ASSERT_TRUE(getClientStream().syncRead(getReactor()));
    ASSERT_EQ(getClientContext().getServerInitialMetadata(), expected);
}

DEATH_TEST_F(MockServerStreamTest, CannotModifyMetadataAfterSent, "invariant") {
    getServerContext().addInitialMetadataEntry("foo", "bar");
    auto serverResponse = makeUniqueMessage();
    ASSERT_TRUE(getServerStream().write(serverResponse.sharedBuffer()));

    getServerContext().addInitialMetadataEntry("cant", "add metadata after it has been sent");
}

DEATH_TEST_F(MockServerStreamTest, CannotRetrieveMetadataBeforeSent, "invariant") {
    getServerContext().addInitialMetadataEntry("foo", "bar");
    getClientContext().getServerInitialMetadata();
}

TEST_F(MockServerStreamTestWithMockedClockSource, DeadlineIsEnforced) {
    clockSource().advance(getTimeout() * 2);
    ASSERT_TRUE(getServerContext().isCancelled());
    ASSERT_FALSE(getServerStream().read());
    ASSERT_FALSE(getServerStream().write(makeUniqueMessage().sharedBuffer()));
    ASSERT_FALSE(getClientStream().syncRead(getReactor()));
    ASSERT_EQ(getClientStream().syncFinish(getReactor()).error_code(),
              ::grpc::StatusCode::DEADLINE_EXCEEDED);
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
        ASSERT_FALSE(getClientStream().syncRead(getReactor()));
    });
}

TEST_F(MockServerStreamTest, TryCancelWrite) {
    getServerContext().tryCancel();
    auto msg = makeUniqueMessage();
    ASSERT_FALSE(getServerStream().write(msg.sharedBuffer()));
    ASSERT_FALSE(getClientStream().syncWrite(getReactor(), msg.sharedBuffer()));
}

TEST_F(MockServerStreamTest, MetadataAvailableAfterTryCancel) {
    auto msg = makeUniqueMessage();
    ASSERT_TRUE(getServerStream().write(msg.sharedBuffer()));
    getServerContext().tryCancel();
    ASSERT_EQ(getServerContext().getClientMetadata(), getClientMetadata());
}

TEST_F(MockServerStreamTestShortTimeout, InitialServerReadTimesOut) {
    ASSERT_FALSE(getServerStream().read());
}

TEST_F(MockServerStreamTestShortTimeout, InitialClientReadTimesOut) {
    ASSERT_FALSE(getClientStream().syncRead(getReactor()));
}

TEST_F(MockServerStreamTest, ClientMetadataIsAccessible) {
    ASSERT_EQ(getServerContext().getClientMetadata(), getClientMetadata());
}

TEST_F(MockServerStreamTest, ClientSideCancellation) {
    ASSERT_TRUE(getClientStream().syncWrite(getReactor(), makeUniqueMessage().sharedBuffer()));
    ASSERT_TRUE(getServerStream().read());

    getClientContext().tryCancel();

    ASSERT_FALSE(getClientStream().syncRead(getReactor()));
    ASSERT_FALSE(getClientStream().syncWrite(getReactor(), makeUniqueMessage().sharedBuffer()));
    ASSERT_FALSE(getServerStream().read());
    ASSERT_FALSE(getServerStream().write(makeUniqueMessage().sharedBuffer()));
    ASSERT_TRUE(getServerContext().isCancelled());

    ASSERT_EQ(getClientStream().syncFinish(getReactor()).error_code(),
              ::grpc::StatusCode::CANCELLED);
}

TEST_F(MockServerStreamTest, CancellationInterruptsFinish) {
    auto pf = makePromiseFuture<::grpc::Status>();
    auto finishThread = stdx::thread(
        [&] { pf.promise.setWith([&] { return getClientStream().syncFinish(getReactor()); }); });
    ON_BLOCK_EXIT([&] { finishThread.join(); });

    // finish() won't return until server end hangs up too.
    ASSERT_FALSE(pf.future.isReady());

    getServerContext().tryCancel();
    ASSERT_EQ(pf.future.get().error_code(), ::grpc::StatusCode::CANCELLED);
}

TEST_F(MockServerStreamTestWithMockedClockSource, DeadlineExceededInterruptsFinish) {
    auto pf = makePromiseFuture<::grpc::Status>();
    auto finishThread = stdx::thread(
        [&] { pf.promise.setWith([&] { return getClientStream().syncFinish(getReactor()); }); });
    ON_BLOCK_EXIT([&] { finishThread.join(); });

    // finish() won't return until server end hangs up too.
    ASSERT_FALSE(pf.future.isReady());

    clockSource().advance(getTimeout() * 2);
    ASSERT_EQ(pf.future.get().error_code(), ::grpc::StatusCode::DEADLINE_EXCEEDED);
}
}  // namespace mongo::transport::grpc
