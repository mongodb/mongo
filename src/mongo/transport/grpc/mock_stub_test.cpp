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
#include <vector>

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/mock_client_context.h"
#include "mongo/transport/grpc/mock_stub.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/scopeguard.h"

namespace mongo::transport::grpc {

class MockStubTest : public LockerNoopServiceContextTest {
public:
    void setUp() override {
        _fixtures = std::make_unique<MockStubTestFixtures>();
    }

    MockStub makeStub() {
        return _fixtures->makeStub();
    }

    MockServer& getServer() {
        return _fixtures->getServer();
    }

    void runEchoTest(std::function<std::shared_ptr<ClientStream>(MockClientContext&)> makeStream) {
        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            getServer().start(monitor, [](auto& rpc) {
                ASSERT_EQ(rpc.serverCtx->getRemote().toString(),
                          MockStubTestFixtures::kClientAddress);
                auto msg = rpc.serverStream->read();
                ASSERT_TRUE(msg);
                ASSERT_TRUE(rpc.serverStream->write(*msg));
                return ::grpc::Status::OK;
            });

            std::vector<stdx::thread> clientThreads;
            for (int i = 0; i < 10; i++) {
                clientThreads.push_back(monitor.spawn([&]() {
                    auto clientMessage = makeUniqueMessage();
                    MockClientContext ctx;
                    auto stream = makeStream(ctx);
                    ASSERT_TRUE(stream->write(clientMessage.sharedBuffer()));

                    auto serverResponse = stream->read();
                    ASSERT_TRUE(serverResponse);
                    ASSERT_EQ_MSG(Message{*serverResponse}, clientMessage);
                }));
            }

            for (auto& thread : clientThreads) {
                thread.join();
            }

            getServer().shutdown();
        });
    }

    std::pair<std::shared_ptr<MockClientStream>, MockRPC> makeRPC(MockClientContext& ctx) {
        auto clientStreamPf = makePromiseFuture<std::shared_ptr<MockClientStream>>();
        auto th = stdx::thread([&, promise = std::move(clientStreamPf.promise)]() mutable {
            promise.setWith([&] {
                auto stub = makeStub();
                return stub.unauthenticatedCommandStream(&ctx);
            });
        });
        ON_BLOCK_EXIT([&th] { th.join(); });
        auto rpc = getServer().acceptRPC();
        ASSERT_TRUE(rpc);
        return {clientStreamPf.future.get(), std::move(*rpc)};
    }

private:
    std::unique_ptr<MockStubTestFixtures> _fixtures;
};

TEST_F(MockStubTest, ConcurrentStreamsAuth) {
    auto stub = makeStub();
    runEchoTest([&](auto& ctx) { return stub.authenticatedCommandStream(&ctx); });
}

TEST_F(MockStubTest, ConcurrentStreamsNoAuth) {
    auto stub = makeStub();
    runEchoTest([&](auto& ctx) { return stub.unauthenticatedCommandStream(&ctx); });
}

TEST_F(MockStubTest, ConcurrentStubsAuth) {
    runEchoTest([&](auto& ctx) { return makeStub().authenticatedCommandStream(&ctx); });
}

TEST_F(MockStubTest, ConcurrentStubsNoAuth) {
    runEchoTest([&](auto& ctx) { return makeStub().unauthenticatedCommandStream(&ctx); });
}

TEST_F(MockStubTest, RPCReturn) {
    const ::grpc::Status kFinalStatus =
        ::grpc::Status{::grpc::StatusCode::FAILED_PRECONDITION, "test"};
    const int kMessageCount = 5;

    MockClientContext ctx;
    auto [clientStream, rpc] = makeRPC(ctx);

    for (auto i = 0; i < kMessageCount; i++) {
        ASSERT_TRUE(rpc.serverStream->write(makeUniqueMessage().sharedBuffer()));
    }

    rpc.sendReturnStatus(kFinalStatus);
    ASSERT_FALSE(rpc.serverCtx->isCancelled())
        << "returning a status should not mark stream as cancelled";
    ASSERT_FALSE(clientStream->write(makeUniqueMessage().sharedBuffer()));

    auto finishPf = makePromiseFuture<::grpc::Status>();
    auto finishThread = stdx::thread(
        [&clientStream = *clientStream, promise = std::move(finishPf.promise)]() mutable {
            promise.setWith([&] { return clientStream.finish(); });
        });
    ON_BLOCK_EXIT([&finishThread] { finishThread.join(); });
    // finish() should not return until all messages have been read.
    ASSERT_FALSE(finishPf.future.isReady());

    // Ensure messages sent before the RPC was finished can still be read.
    for (auto i = 0; i < kMessageCount; i++) {
        ASSERT_TRUE(clientStream->read());
    }
    ASSERT_FALSE(clientStream->read());

    // Ensure that finish() returns now that all the messages have been read.
    auto status = finishPf.future.get();
    ASSERT_EQ(status.error_code(), kFinalStatus.error_code());

    // Cancelling a finished RPC should have no effect.
    rpc.serverCtx->tryCancel();
    ASSERT_FALSE(rpc.serverCtx->isCancelled());
}

TEST_F(MockStubTest, RPCCancellation) {
    const ::grpc::Status kCancellationStatus =
        ::grpc::Status{::grpc::StatusCode::UNAVAILABLE, "mock network error"};

    MockClientContext ctx;
    auto [clientStream, rpc] = makeRPC(ctx);

    ASSERT_TRUE(clientStream->write(makeUniqueMessage().sharedBuffer()));

    rpc.cancel(kCancellationStatus);

    ASSERT_TRUE(rpc.serverCtx->isCancelled());
    ASSERT_FALSE(clientStream->write(makeUniqueMessage().sharedBuffer()));
    ASSERT_FALSE(clientStream->read());
    ASSERT_EQ(clientStream->finish().error_code(), kCancellationStatus.error_code());
}

TEST_F(MockStubTest, CannotReturnStatusForCancelledRPC) {
    MockClientContext ctx;
    auto [clientStream, rpc] = makeRPC(ctx);

    ASSERT_TRUE(clientStream->write(makeUniqueMessage().sharedBuffer()));

    rpc.cancel(::grpc::Status::CANCELLED);
    rpc.sendReturnStatus(::grpc::Status::OK);

    ASSERT_TRUE(rpc.serverCtx->isCancelled());
    ASSERT_FALSE(clientStream->write(makeUniqueMessage().sharedBuffer()));
    ASSERT_FALSE(clientStream->read());
    ASSERT_EQ(clientStream->finish().error_code(), ::grpc::StatusCode::CANCELLED);
}

}  // namespace mongo::transport::grpc
