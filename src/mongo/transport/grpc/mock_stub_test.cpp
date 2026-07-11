// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/mock_stub.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/grpc/mock_client_context.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/scopeguard.h"

#include <memory>
#include <vector>

namespace mongo::transport::grpc {

class MockStubTest : public ServiceContextTest {
public:
    void setUp() override {
        _fixtures = std::make_unique<MockStubTestFixtures>();
        _reactor = std::make_shared<GRPCReactor>();
    }

    MockStub makeStub() {
        return _fixtures->makeStub();
    }

    MockServer& getServer() {
        return _fixtures->getServer();
    }

    const std::shared_ptr<GRPCReactor>& getReactor() {
        return _reactor;
    }

    void runEchoTest(
        std::function<std::shared_ptr<MockClientStream>(MockClientContext&)> makeStream) {
        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            getServer().start(
                monitor,
                [](auto session) {
                    ASSERT_EQ(session->remote().toString(), MockStubTestFixtures::kClientAddress);
                    auto msg = uassertStatusOK(session->sourceMessage());
                    ASSERT_OK(session->sinkMessage(msg));
                    session->setTerminationStatus(Status::OK());
                },
                std::make_shared<WireVersionProvider>());

            std::vector<stdx::thread> clientThreads;
            for (int i = 0; i < 10; i++) {
                clientThreads.push_back(monitor.spawn([&]() {
                    auto clientMessage = makeUniqueMessage();
                    MockClientContext ctx;
                    ctx.addMetadataEntry(
                        std::string{util::constants::kWireVersionKey},
                        std::to_string(WireSpec::getWireSpec(getGlobalServiceContext())
                                           .getIncomingExternalClient()
                                           .maxWireVersion));
                    ctx.addMetadataEntry(std::string{util::constants::kAuthenticationTokenKey},
                                         "my-token");
                    auto stream = makeStream(ctx);
                    ASSERT_TRUE(stream->syncWrite(getReactor(), clientMessage.sharedBuffer()));

                    auto serverResponse = stream->syncRead(getReactor());
                    ASSERT_TRUE(serverResponse);
                    ASSERT_EQ_MSG(Message{*serverResponse}, clientMessage);
                    ASSERT_EQ(stream->syncFinish(getReactor()).error_code(), ::grpc::OK);
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
                return stub.unauthenticatedCommandStream(&ctx, getReactor());
            });
        });
        ON_BLOCK_EXIT([&th] { th.join(); });
        auto rpc = getServer().acceptRPC();
        ASSERT_TRUE(rpc);
        return {clientStreamPf.future.get(), std::move(*rpc)};
    }

private:
    std::unique_ptr<MockStubTestFixtures> _fixtures;
    std::shared_ptr<GRPCReactor> _reactor;
};

TEST_F(MockStubTest, ConcurrentStreamsAuth) {
    auto stub = makeStub();
    runEchoTest([&](auto& ctx) { return stub.authenticatedCommandStream(&ctx, getReactor()); });
}

TEST_F(MockStubTest, ConcurrentStreamsNoAuth) {
    auto stub = makeStub();
    runEchoTest([&](auto& ctx) { return stub.unauthenticatedCommandStream(&ctx, getReactor()); });
}

TEST_F(MockStubTest, ConcurrentStubsAuth) {
    runEchoTest(
        [&](auto& ctx) { return makeStub().authenticatedCommandStream(&ctx, getReactor()); });
}

TEST_F(MockStubTest, ConcurrentStubsNoAuth) {
    runEchoTest(
        [&](auto& ctx) { return makeStub().unauthenticatedCommandStream(&ctx, getReactor()); });
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
    ASSERT_FALSE(clientStream->syncWrite(getReactor(), makeUniqueMessage().sharedBuffer()));

    auto finishPf = makePromiseFuture<::grpc::Status>();
    auto finishThread = stdx::thread([this,
                                      clientStream = clientStream,
                                      promise = std::move(finishPf.promise)]() mutable {
        promise.setWith([this, clientStream] { return clientStream->syncFinish(getReactor()); });
    });
    ON_BLOCK_EXIT([&finishThread] { finishThread.join(); });
    // finish() should not return until all messages have been read.
    ASSERT_FALSE(finishPf.future.isReady());

    // Ensure messages sent before the RPC was finished can still be read.
    for (auto i = 0; i < kMessageCount; i++) {
        ASSERT_TRUE(clientStream->syncRead(getReactor()));
    }
    ASSERT_FALSE(clientStream->syncRead(getReactor()));

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

    ASSERT_TRUE(clientStream->syncWrite(getReactor(), makeUniqueMessage().sharedBuffer()));

    rpc.cancel(kCancellationStatus);

    ASSERT_TRUE(rpc.serverCtx->isCancelled());
    ASSERT_FALSE(clientStream->syncWrite(getReactor(), makeUniqueMessage().sharedBuffer()));
    ASSERT_FALSE(clientStream->syncRead(getReactor()));
    ASSERT_EQ(clientStream->syncFinish(getReactor()).error_code(),
              kCancellationStatus.error_code());
}

TEST_F(MockStubTest, CannotReturnStatusForCancelledRPC) {
    MockClientContext ctx;
    auto [clientStream, rpc] = makeRPC(ctx);

    ASSERT_TRUE(clientStream->syncWrite(getReactor(), makeUniqueMessage().sharedBuffer()));

    rpc.cancel(::grpc::Status::CANCELLED);
    rpc.sendReturnStatus(::grpc::Status::OK);

    ASSERT_TRUE(rpc.serverCtx->isCancelled());
    ASSERT_FALSE(clientStream->syncWrite(getReactor(), makeUniqueMessage().sharedBuffer()));
    ASSERT_FALSE(clientStream->syncRead(getReactor()));
    ASSERT_EQ(clientStream->syncFinish(getReactor()).error_code(), ::grpc::StatusCode::CANCELLED);
}

}  // namespace mongo::transport::grpc
