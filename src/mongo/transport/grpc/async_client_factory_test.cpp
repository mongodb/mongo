/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/grpc/async_client_factory.h"
#include "mongo/transport/grpc/client.h"
#include "mongo/transport/grpc/grpc_session_manager.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/grpc_transport_layer_impl.h"
#include "mongo/transport/grpc/grpc_transport_layer_mock.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/session_workflow_test_util.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {

// TODO SERVER-100051: share this setup logic with other tests using GRPCTransportLayer.
class GRPCAsyncClientFactoryTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        auto svcCtx = getServiceContext();

        svcCtx->setPeriodicRunner(makePeriodicRunner(getServiceContext()));

        sslGlobalParams.sslCAFile = CommandServiceTestFixtures::kCAFile;
        sslGlobalParams.sslPEMKeyFile = CommandServiceTestFixtures::kServerCertificateKeyFile;
        sslGlobalParams.sslMode.store(SSLParams::SSLModes::SSLMode_requireSSL);

        auto clientCache = std::make_shared<ClientCache>();

        _tl = std::make_unique<GRPCTransportLayerImpl>(
            svcCtx,
            CommandServiceTestFixtures::makeTLOptions(),
            std::make_unique<GRPCSessionManager>(svcCtx, clientCache));

        uassertStatusOK(_tl->registerService(std::make_unique<CommandService>(
            _tl.get(),
            [&](std::shared_ptr<Session> session) {
                while (true) {
                    try {
                        OpMsg msg;
                        auto recvMsg = uassertStatusOK(session->sourceMessage());
                        LOGV2(9936108, "Received message", "msg"_attr = recvMsg.opMsgDebugString());

                        mongo::BSONObjBuilder builder;
                        builder.append("ok", 1);
                        msg.body = builder.obj();
                        auto out = msg.serialize();
                        out.header().setId(nextMessageId());
                        out.header().setResponseToMsgId(recvMsg.header().getId());
                        uassertStatusOK(session->sinkMessage(out));
                    } catch (ExceptionFor<ErrorCodes::StreamTerminated>&) {
                        return;
                    }
                }
            },
            std::make_unique<WireVersionProvider>(),
            clientCache)));
        uassertStatusOK(_tl->setup());
        uassertStatusOK(_tl->start());

        _reactor = _tl->getReactor(TransportLayer::WhichReactor::kNewReactor);
        _reactorThread = stdx::thread([&] {
            _reactor->run();
            _reactor->drain();
        });

        _factory = std::make_unique<GRPCAsyncClientFactory>();
        _factory->startup(svcCtx, _tl.get(), _reactor);
    }

    void tearDown() override {
        _factory->shutdown();
        _reactor->stop();
        _reactorThread.join();
        _tl->shutdown();
        ServiceContextTest::tearDown();
    }

    executor::AsyncClientFactory& getFactory() {
        return *_factory;
    }

    GRPCTransportLayer& getTransportLayer() {
        return *_tl;
    }

    const HostAndPort& getTarget() {
        return _tl->getListeningAddresses()[0];
    }

    std::shared_ptr<executor::AsyncClientFactory::AsyncClientHandle> getClient() {
        return getClient(getTarget());
    }

    std::shared_ptr<executor::AsyncClientFactory::AsyncClientHandle> getClient(
        const HostAndPort& target) {
        return getFactory()
            .get(target,
                 ConnectSSLMode::kGlobalSSLMode,
                 CommandServiceTestFixtures::kDefaultConnectTimeout)
            .get();
    }

    void waitForDisconnected(
        const std::shared_ptr<executor::AsyncClientFactory::AsyncClientHandle>& handle) {
        size_t retries = 3;
        while (handle->getClient().isStillConnected() && retries-- > 0) {
            sleepmillis(100);
        }
    }

    void shutdownAndAssertOnTransportStats(int successfulStreams, int failedStreams) {
        // Wait for the factory to shutdown to ensure all clients are destroyed.
        getFactory().shutdown();

        BSONObjBuilder stats;
        _tl->appendStatsForServerStatus(&stats);
        auto egressStats = stats.obj();
        ASSERT_EQ(
            egressStats[kStreamsSubsectionFieldName][kSuccessfulStreamsFieldName].numberLong(),
            successfulStreams);
        ASSERT_EQ(egressStats[kStreamsSubsectionFieldName][kFailedStreamsFieldName].numberLong(),
                  failedStreams);
    }

private:
    std::unique_ptr<GRPCTransportLayer> _tl;
    std::unique_ptr<executor::AsyncClientFactory> _factory;

    stdx::thread _reactorThread;
    ReactorHandle _reactor;

    unittest::MinimumLoggedSeverityGuard logSeverityGuardNetwork{
        logv2::LogComponent::kNetwork,
        logv2::LogSeverity::Debug(GRPCAsyncClientFactory::kDiagnosticLogLevel)};
    test::SSLGlobalParamsGuard _sslGlobalParamsGuard;
};

TEST_F(GRPCAsyncClientFactoryTest, Ping) {
    {
        auto handle = getClient();
        ON_BLOCK_EXIT([&] { handle->indicateSuccess(); });
        auto msg = makeUniqueMessage();
        auto resp = handle->getClient().runCommand(OpMsgRequest::parse(msg)).get();
        ASSERT_OK(getStatusFromCommandResult(resp->getCommandReply()));
        handle->indicateSuccess();
    }

    shutdownAndAssertOnTransportStats(1 /*successful streams*/, 0 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, ConcurrentUsage) {
    const int concurrentThreads = CommandServiceTestFixtures::kMaxThreads - 10;
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        std::vector<stdx::thread> threads;

        for (int i = 0; i < concurrentThreads; i++) {
            auto th = monitor.spawn([&] {
                auto handle = getClient();
                for (int req = 0; req < 5; req++) {
                    auto msg = makeUniqueMessage();
                    auto resp = handle->getClient().runCommand(OpMsgRequest::parse(msg)).get();
                    ON_BLOCK_EXIT([&] { handle->indicateSuccess(); });
                    ASSERT_OK(getStatusFromCommandResult(resp->getCommandReply()));
                }
            });
            threads.push_back(std::move(th));
        }

        for (auto&& thread : threads) {
            thread.join();
        };
    });

    shutdownAndAssertOnTransportStats(concurrentThreads /*successful streams*/,
                                      0 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, DropAllConnections) {
    {
        auto handle1 = getClient();
        ON_BLOCK_EXIT([&] { handle1->indicateSuccess(); });
        auto handle2 = getClient();
        ON_BLOCK_EXIT([&] { handle2->indicateSuccess(); });

        getFactory().dropConnections();
        auto msg = OpMsgRequest::parseOwned(makeUniqueMessage());
        ASSERT_EQ(handle1->getClient().runCommand(msg).getNoThrow(), ErrorCodes::CallbackCanceled);
        ASSERT_EQ(handle2->getClient().runCommand(msg).getNoThrow(), ErrorCodes::CallbackCanceled);
    }

    shutdownAndAssertOnTransportStats(0 /*successful streams*/, 2 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, KeepOpen) {
    {
        auto handle = getClient();
        ON_BLOCK_EXIT([&] { handle->indicateSuccess(); });
        getFactory().setKeepOpen(handle->getClient().remote(), true);
        getFactory().dropConnections();
        auto msg = OpMsgRequest::parseOwned(makeUniqueMessage());
        ASSERT_OK(handle->getClient().runCommand(msg).getNoThrow());
    }

    shutdownAndAssertOnTransportStats(1 /*successful streams*/, 0 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, Shutdown) {
    auto pf = makePromiseFuture<void>();
    stdx::thread shutdownThread;

    {
        auto handle1 = getClient();
        ON_BLOCK_EXIT([&] { handle1->indicateSuccess(); });
        auto handle2 = getClient();
        ON_BLOCK_EXIT([&] { handle2->indicateSuccess(); });

        shutdownThread = stdx::thread([&] {
            getFactory().shutdown();
            pf.promise.emplaceValue();
        });

        waitForDisconnected(handle1);
        waitForDisconnected(handle2);
        auto msg = OpMsgRequest::parseOwned(makeUniqueMessage());
        ASSERT_EQ(handle1->getClient().runCommand(msg).getNoThrow(), ErrorCodes::CallbackCanceled);
        ASSERT_EQ(handle2->getClient().runCommand(msg).getNoThrow(), ErrorCodes::CallbackCanceled);

        ASSERT_FALSE(pf.future.isReady());
    }

    pf.future.get();
    shutdownThread.join();
    shutdownAndAssertOnTransportStats(0 /*successful streams*/, 2 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, RefuseShutdownWithActiveClient) {
    auto pf = makePromiseFuture<void>();
    stdx::thread shutdownThread;
    {
        auto handle1 = getClient();
        ON_BLOCK_EXIT([&] { handle1->indicateSuccess(); });

        shutdownThread = stdx::thread([&] {
            getFactory().shutdown();
            pf.promise.emplaceValue();
        });
        waitForDisconnected(handle1);
        auto msg = OpMsgRequest::parseOwned(makeUniqueMessage());
        ASSERT_EQ(handle1->getClient().runCommand(msg).getNoThrow(), ErrorCodes::CallbackCanceled);

        ASSERT_FALSE(pf.future.isReady());

        auto client = ServiceContextTest::getClient();
        auto opCtx = getServiceContext()->makeOperationContext(client);
        opCtx->setDeadlineAfterNowBy(Milliseconds(500), ErrorCodes::ExceededTimeLimit);
        ASSERT_THROWS_CODE(pf.future.get(opCtx.get()), DBException, ErrorCodes::ExceededTimeLimit);

        // Now destroy the client so that shutdown can succeed.
    }
    pf.future.get();
    shutdownThread.join();
}

class MockGRPCAsyncClientFactoryTest : public MockGRPCTransportLayerTest {
public:
    void setUp() override {
        MockGRPCTransportLayerTest::setUp();

        _net = executor::makeNetworkInterfaceWithClientFactory(
            "MockGRPCAsyncClientFactoryTest", std::make_shared<GRPCAsyncClientFactory>());
        _net->startup();
    }

    void tearDown() override {
        _net->shutdown();
        MockGRPCTransportLayerTest::tearDown();
    }

    executor::NetworkInterface& getNet() {
        return *_net;
    }

    void runStreamEstablishmentTimeoutTest(boost::optional<ErrorCodes::Error> customCode);

private:
    std::unique_ptr<executor::NetworkInterface> _net;
};


TEST_F(MockGRPCAsyncClientFactoryTest, ConnectionErrorAssociatedWithRemote) {
    FailPointEnableBlock fpb("grpcFailChannelEstablishment");

    auto target = HostAndPort("localhost", 12345);
    auto cb = executor::TaskExecutor::CallbackHandle();
    executor::RemoteCommandRequest request(
        target, DatabaseName::kAdmin, BSON("ping" << 1), nullptr);
    auto result = getNet().startCommand(cb, request).get();

    ASSERT_EQ(ErrorCodes::HostUnreachable, result.status);
    ASSERT_EQ(result.target, target);
}

TEST_F(MockGRPCAsyncClientFactoryTest, CancelStreamEstablishment) {
    boost::optional<FailPointEnableBlock> fpb("grpcHangOnStreamEstablishment");
    auto cbh = executor::TaskExecutor::CallbackHandle();
    CancellationSource cancellationSource;

    auto pf = makePromiseFuture<executor::RemoteCommandResponse>();

    executor::RemoteCommandRequest request(HostAndPort("localhost", 12345),
                                           DatabaseName::kAdmin,
                                           BSON("ping" << 1),
                                           nullptr,
                                           Minutes(1));
    auto cmdThread = stdx::thread([&] {
        pf.promise.setFrom(
            getNet().startCommand(cbh, request, nullptr, cancellationSource.token()).getNoThrow());
    });
    ON_BLOCK_EXIT([&] { cmdThread.join(); });

    fpb.get()->waitForTimesEntered(fpb->initialTimesEntered() + 1);
    cancellationSource.cancel();
    fpb.reset();

    ASSERT_EQ(getNet().getCounters().sent, 0);

    // Wait for op to complete, assert that it was canceled.
    auto result = pf.future.get();
    ASSERT_EQ(ErrorCodes::CallbackCanceled, result.status);
    ASSERT(result.elapsed);

    auto counters = getNet().getCounters();
    ASSERT_EQ(1, counters.canceled);
    ASSERT_EQ(0, counters.timedOut);
    ASSERT_EQ(0, counters.failed);
    ASSERT_EQ(0, counters.succeeded);
    ASSERT_EQ(0, counters.failedRemotely);
}

void MockGRPCAsyncClientFactoryTest::runStreamEstablishmentTimeoutTest(
    boost::optional<ErrorCodes::Error> customCode) {
    FailPointEnableBlock fpb("grpcHangOnStreamEstablishment");

    auto cbh = executor::TaskExecutor::CallbackHandle();
    executor::RemoteCommandRequest request(HostAndPort("localhost", 12345),
                                           DatabaseName::kAdmin,
                                           BSON("ping" << 1),
                                           nullptr,
                                           Milliseconds(100));
    request.timeoutCode = customCode;
    auto res = getNet().startCommand(cbh, request).get();
    ASSERT_EQ(res.status, customCode.value_or(ErrorCodes::NetworkTimeout));
    ASSERT_EQ(getNet().getCounters().sent, 0);

    auto counters = getNet().getCounters();
    ASSERT_EQ(0, counters.canceled);
    ASSERT_EQ(1, counters.timedOut);
    ASSERT_EQ(0, counters.failed);
    ASSERT_EQ(0, counters.succeeded);
    ASSERT_EQ(0, counters.failedRemotely);
}

TEST_F(MockGRPCAsyncClientFactoryTest, TimeoutStreamEstablishment) {
    runStreamEstablishmentTimeoutTest({});
}

TEST_F(MockGRPCAsyncClientFactoryTest, TimeoutStreamEstablishmentCustomCode) {
    runStreamEstablishmentTimeoutTest(ErrorCodes::MaxTimeMSExpired);
}
}  // namespace mongo::transport::grpc
