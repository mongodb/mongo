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

#include "mongo/executor/async_client_factory.h"

#include "mongo/db/service_context_test_fixture.h"
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
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/transport/session_workflow_test_util.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
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
        auto tlOptions = CommandServiceTestFixtures::makeTLOptions();
        // Set up a few listening addresses to test different targets.
        tlOptions.bindIpList = {"localhost", "127.0.0.1", "::1"};

        _tl = std::make_unique<GRPCTransportLayerImpl>(
            svcCtx, tlOptions, std::make_unique<GRPCSessionManager>(svcCtx, clientCache));

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

        _factory = std::make_unique<GRPCAsyncClientFactory>("AsyncClientFactoryTest");
        _factory->startup(svcCtx, _tl.get(), _reactor);
    }

    void tearDown() override {
        _factory->shutdown();
        _factory.reset();
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

    const ReactorHandle& getReactor() {
        return _reactor;
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

    std::shared_ptr<executor::AsyncClientFactory::AsyncClientHandle> getLeasedClient() {
        return getLeasedClient(getTarget());
    }

    std::shared_ptr<executor::AsyncClientFactory::AsyncClientHandle> getLeasedClient(
        const HostAndPort& target, bool lease = false) {
        return getFactory()
            .lease(target,
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

    UUID getChannelIdForClient(
        const std::shared_ptr<executor::AsyncClientFactory::AsyncClientHandle>& client) {
        return checked_cast<EgressSession&>(client->getClient().getTransportSession())
            .getChannelId();
    }

    void shutdownAndAssertOnTransportStats(int successfulStreams, int failedStreams) {
        // Wait for the factory to shutdown to ensure all clients are destroyed.
        getFactory().shutdown();

        BSONObjBuilder stats;
        _tl->appendStatsForServerStatus(&stats);
        auto egressStats = stats.obj();
        ASSERT_EQ(egressStats[kStreamsSubsectionFieldName]
                             [GRPCConnectionStats::kTotalSuccessfulStreamsFieldName]
                                 .numberLong(),
                  successfulStreams);
        ASSERT_EQ(egressStats[kStreamsSubsectionFieldName]
                             [GRPCConnectionStats::kTotalFailedStreamsFieldName]
                                 .numberLong(),
                  failedStreams);
    }

    void assertStatsSoon(int created, int inUse, int leased, int open) {
        GRPCConnectionStats stats;
        bool result = false;

        int iterations = 10;
        for (int i = 0; i < iterations; ++i) {
            stats = checked_cast<GRPCAsyncClientFactory*>(_factory.get())->getStats();
            if (stats.getTotalStreamsCreated() == created &&
                stats.getTotalInUseStreams() == inUse && stats.getTotalLeasedStreams() == leased &&
                stats.getTotalOpenChannels() == open) {
                result = true;
                break;
            } else if (i == iterations - 1) {
                break;
            }

            sleepmillis(100);
        }

        if (!result) {
            LOGV2(9924602,
                  "Failing AsyncClientFactory stats assertion",
                  "created"_attr = stats.getTotalStreamsCreated(),
                  "expectedCreated"_attr = created,
                  "inUse"_attr = stats.getTotalInUseStreams(),
                  "expectedInUse"_attr = inUse,
                  "leased"_attr = stats.getTotalLeasedStreams(),
                  "expectedLeased"_attr = leased,
                  "open"_attr = stats.getTotalOpenChannels(),
                  "expectedOpen"_attr = open);
        }
        ASSERT_TRUE(result);
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
        auto handle1ChannelId = getChannelIdForClient(handle1);
        ON_BLOCK_EXIT([&] { handle1->indicateSuccess(); });
        auto handle2 = getClient();
        ON_BLOCK_EXIT([&] { handle2->indicateSuccess(); });

        ASSERT_EQ(handle1ChannelId, getChannelIdForClient(handle2));

        getFactory().dropConnections();
        auto msg = OpMsgRequest::parseOwned(makeUniqueMessage());
        ASSERT_EQ(handle1->getClient().runCommand(msg).getNoThrow(), ErrorCodes::CallbackCanceled);
        ASSERT_EQ(handle2->getClient().runCommand(msg).getNoThrow(), ErrorCodes::CallbackCanceled);

        // New sessions succeed and use a different underlying channel.
        auto handle3 = getClient();
        ON_BLOCK_EXIT([&] { handle3->indicateSuccess(); });

        ASSERT_NE(handle1ChannelId, getChannelIdForClient(handle3));

        ASSERT_OK(handle3->getClient().runCommand(msg).getNoThrow());
    }

    shutdownAndAssertOnTransportStats(1 /*successful streams*/, 2 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, DropConnectionToTarget) {
    {
        auto target1 = getTransportLayer().getListeningAddresses()[0];
        auto handle1 = getClient(target1);
        ON_BLOCK_EXIT([&] { handle1->indicateSuccess(); });

        auto target2 = getTransportLayer().getListeningAddresses()[1];
        auto handle2 = getClient(target2);
        ON_BLOCK_EXIT([&] { handle2->indicateSuccess(); });

        // They are using different channels because they are connected to different remotes.
        ASSERT_NE(getChannelIdForClient(handle1), getChannelIdForClient(handle2));

        getFactory().dropConnections(target1);

        auto msg = OpMsgRequest::parseOwned(makeUniqueMessage());
        ASSERT_EQ(handle1->getClient().runCommand(msg).getNoThrow(), ErrorCodes::CallbackCanceled);

        // The other target is unaffected by dropConnections.
        ASSERT_OK(handle2->getClient().runCommand(msg).getNoThrow());
    }

    shutdownAndAssertOnTransportStats(1 /*successful streams*/, 1 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, DropConnectionsWhileConnecting) {
    {
        // Start a stream establishment attempt that will hang until cancelled.
        auto connAttempt = getFactory().get(
            HostAndPort("localhost", 12345), ConnectSSLMode::kGlobalSSLMode, Milliseconds::max());
        assertStatsSoon(0 /*created*/, 0 /*inUse*/, 0 /*leased*/, 1 /*open*/);
        getFactory().dropConnections();
        auto status = connAttempt.getNoThrow().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status.code(), ErrorCodes::CallbackCanceled);
        ASSERT_EQ(status.reason(),
                  "Cancelled stream establishment due to dropping the associated connection");
    }

    shutdownAndAssertOnTransportStats(0 /*successful streams*/, 0 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, DropConnectionsWhileConnectingWithKeepOpen) {
    {
        CancellationSource cancelSource;
        auto remote = HostAndPort("localhost", 12345);
        getFactory().setKeepOpen(remote, true);

        // Start a stream establishment attempt that will hang until cancelled.
        auto connAttempt = getFactory().get(
            remote, ConnectSSLMode::kGlobalSSLMode, Milliseconds::max(), cancelSource.token());
        assertStatsSoon(0 /*created*/, 0 /*inUse*/, 0 /*leased*/, 1 /*open*/);
        getFactory().dropConnections();

        // The first dropConnections does not affect the connect Future.
        ASSERT_FALSE(connAttempt.isReady());

        // Cancel the cancellation token to terminate the stream establishment.
        cancelSource.cancel();
        auto status = connAttempt.getNoThrow().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQ(status.code(), ErrorCodes::CallbackCanceled);
        ASSERT_EQ(status.reason(), "gRPC stream establishment was cancelled");
    }

    shutdownAndAssertOnTransportStats(0 /*successful streams*/, 0 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, KeepOpen) {
    {
        auto handle1 = getClient();
        ON_BLOCK_EXIT([&] { handle1->indicateSuccess(); });
        getFactory().setKeepOpen(handle1->getClient().remote(), true);

        // Drop all connections to check that keep open applied.
        getFactory().dropConnections();

        auto msg = OpMsgRequest::parseOwned(makeUniqueMessage());
        // We can still run a command on the old session.
        ASSERT_OK(handle1->getClient().runCommand(msg).getNoThrow());

        // New sessions are also unaffected.
        auto handle2 = getClient();
        ON_BLOCK_EXIT([&] { handle2->indicateSuccess(); });
        ASSERT_OK(handle2->getClient().runCommand(msg).getNoThrow());

        // The same channel is used for the remote on new sessions because it was kept open.
        ASSERT_EQ(getChannelIdForClient(handle1), getChannelIdForClient(handle2));
    }

    shutdownAndAssertOnTransportStats(2 /*successful streams*/, 0 /*failed streams*/);
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

TEST_F(GRPCAsyncClientFactoryTest, PerClientStatsTest) {
    auto handle = getClient();
    auto anotherHandle = getClient();
    auto leasedHandle = getLeasedClient();
    assertStatsSoon(3 /*created*/, 2 /*inUse*/, 1 /*leased*/, 1 /*open*/);

    // Destroy two handles.
    handle->indicateSuccess();
    leasedHandle->indicateFailure({ErrorCodes::CallbackCanceled, "Destroying leased client"});
    handle.reset();
    leasedHandle.reset();

    // Handle outcome should not affect stats.
    assertStatsSoon(3 /*created*/, 1 /*inUse*/, 0 /*leased*/, 1 /*open*/);

    auto msg = makeUniqueMessage();
    auto resp = anotherHandle->getClient().runCommand(OpMsgRequest::parse(msg)).get();
    ASSERT_OK(getStatusFromCommandResult(resp->getCommandReply()));

    // A handle is still in use after a command is run and before it is destroyed.
    assertStatsSoon(3 /*created*/, 1 /*inUse*/, 0 /*leased*/, 1 /*open*/);

    anotherHandle->indicateSuccess();
    anotherHandle.reset();
    assertStatsSoon(3 /*created*/, 0 /*inUse*/, 0 /*leased*/, 1 /*open*/);
}

TEST_F(GRPCAsyncClientFactoryTest, CumulativeStatsTest) {
    std::unique_ptr<GRPCAsyncClientFactory> factory1;
    std::unique_ptr<GRPCAsyncClientFactory> factory2;
    {
        factory1 = std::make_unique<GRPCAsyncClientFactory>("AsyncClientFactoryTest-1");
        factory2 = std::make_unique<GRPCAsyncClientFactory>("AsyncClientFactoryTest-2");
        factory1->startup(getServiceContext(), &getTransportLayer(), getReactor());
        factory2->startup(getServiceContext(), &getTransportLayer(), getReactor());
        ON_BLOCK_EXIT([&] {
            factory1->shutdown();
            factory2->shutdown();
        });

        auto client1 = factory1
                           ->get(getTarget(),
                                 ConnectSSLMode::kGlobalSSLMode,
                                 CommandServiceTestFixtures::kDefaultConnectTimeout)
                           .get();
        ON_BLOCK_EXIT([&] { client1->indicateSuccess(); });

        auto client2 = factory2
                           ->get(getTarget(),
                                 ConnectSSLMode::kGlobalSSLMode,
                                 CommandServiceTestFixtures::kDefaultConnectTimeout)
                           .get();
        ON_BLOCK_EXIT([&] { client2->indicateSuccess(); });

        // Assert that at the factory level, we only show stats per client.
        GRPCConnectionStats stats1 = factory1->getStats();
        ASSERT_EQ(stats1.getTotalStreamsCreated(), 1);
        ASSERT_EQ(stats1.getTotalInUseStreams(), 1);
        ASSERT_EQ(stats1.getTotalLeasedStreams(), 0);
        ASSERT_EQ(stats1.getTotalOpenChannels(), 1);

        GRPCConnectionStats stats2 = factory2->getStats();
        ASSERT_EQ(stats2.getTotalStreamsCreated(), 1);
        ASSERT_EQ(stats2.getTotalInUseStreams(), 1);
        ASSERT_EQ(stats2.getTotalLeasedStreams(), 0);
        ASSERT_EQ(stats2.getTotalOpenChannels(), 1);

        // At the TL level, we see all channels and streams.
        BSONObjBuilder stats;
        getTransportLayer().appendStatsForServerStatus(&stats);
        auto egressStats = stats.obj();
        ASSERT_EQ(egressStats[GRPCConnectionStats::kTotalOpenChannelsFieldName].numberLong(), 2);
        ASSERT_EQ(egressStats[kStreamsSubsectionFieldName]
                             [GRPCConnectionStats::kTotalActiveStreamsFieldName]
                                 .numberLong(),
                  2);
    }
    // And we also count 2 successful streams at the TL level.
    shutdownAndAssertOnTransportStats(2 /*successful streams*/, 0 /*failed streams*/);
}

class MockGRPCAsyncClientFactoryTest : public MockGRPCTransportLayerTest {
public:
    void setUp() override {
        MockGRPCTransportLayerTest::setUp();

        _net = executor::makeNetworkInterfaceWithClientFactory(
            "MockGRPCAsyncClientFactoryTest",
            std::make_shared<GRPCAsyncClientFactory>("MockGRPCAsyncClientFactoryTest"));
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

TEST_F(MockGRPCAsyncClientFactoryTest, ConnectionErrorAssociatedWithRemoteWithDeadline) {
    FailPointEnableBlock fpb("grpcFailChannelEstablishment");

    auto target = HostAndPort("localhost", 12345);
    auto cb = executor::TaskExecutor::CallbackHandle();
    executor::RemoteCommandRequest request(
        target, DatabaseName::kAdmin, BSON("ping" << 1), nullptr, Milliseconds(100));
    auto result = getNet().startCommand(cb, request).get();

    ASSERT_EQ(ErrorCodes::HostUnreachable, result.status);
    ASSERT_EQ(result.target, target);
}

TEST_F(MockGRPCAsyncClientFactoryTest, CancelChannelEstablishment) {
    boost::optional<FailPointEnableBlock> fpb("grpcHangOnChannelEstablishment");
    auto cbh = executor::TaskExecutor::CallbackHandle();
    CancellationSource cancellationSource;

    auto pf = makePromiseFuture<executor::RemoteCommandResponse>();

    executor::RemoteCommandRequest request(
        HostAndPort("localhost", 12345), DatabaseName::kAdmin, BSON("ping" << 1), nullptr);
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
}

TEST_F(MockGRPCAsyncClientFactoryTest, CancelStreamEstablishment) {
    runTestWithMockServer([this]() {
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
            pf.promise.setFrom(getNet()
                                   .startCommand(cbh, request, nullptr, cancellationSource.token())
                                   .getNoThrow());
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
    });
}

TEST_F(MockGRPCAsyncClientFactoryTest, FailStreamEstablishment) {
    runTestWithMockServer([this]() {
        FailPointEnableBlock fpb("grpcFailStreamEstablishment");

        auto cbh = executor::TaskExecutor::CallbackHandle();
        executor::RemoteCommandRequest request(
            HostAndPort("localhost", 12345), DatabaseName::kAdmin, BSON("ping" << 1), nullptr);
        auto res = getNet().startCommand(cbh, request).get();
        ASSERT_EQ(res.status, ErrorCodes::HostUnreachable);
        ASSERT_EQ(getNet().getCounters().sent, 0);

        auto counters = getNet().getCounters();
        ASSERT_EQ(0, counters.canceled);
        ASSERT_EQ(0, counters.timedOut);
        ASSERT_EQ(1, counters.failed);
        ASSERT_EQ(0, counters.succeeded);
        ASSERT_EQ(0, counters.failedRemotely);
    });
}

void MockGRPCAsyncClientFactoryTest::runStreamEstablishmentTimeoutTest(
    boost::optional<ErrorCodes::Error> customCode) {
    runTestWithMockServer([&, this]() {
        FailPointEnableBlock fpb("grpcHangOnStreamEstablishment");

        auto cbh = executor::TaskExecutor::CallbackHandle();
        executor::RemoteCommandRequest request(HostAndPort("localhost", 12345),
                                               DatabaseName::kAdmin,
                                               BSON("ping" << 1),
                                               nullptr,
                                               Milliseconds(100));
        request.timeoutCode = customCode;
        auto res = getNet().startCommand(cbh, request).get();
        ASSERT_EQ(res.status, customCode.value_or(ErrorCodes::ExceededTimeLimit));
        ASSERT_EQ(getNet().getCounters().sent, 0);

        auto counters = getNet().getCounters();
        ASSERT_EQ(0, counters.canceled);
        ASSERT_EQ(1, counters.timedOut);
        ASSERT_EQ(0, counters.failed);
        ASSERT_EQ(0, counters.succeeded);
        ASSERT_EQ(0, counters.failedRemotely);
    });
}

TEST_F(MockGRPCAsyncClientFactoryTest, TimeoutStreamEstablishment) {
    runStreamEstablishmentTimeoutTest({});
}

TEST_F(MockGRPCAsyncClientFactoryTest, TimeoutStreamEstablishmentCustomCode) {
    runStreamEstablishmentTimeoutTest(ErrorCodes::MaxTimeMSExpired);
}
}  // namespace mongo::transport::grpc
