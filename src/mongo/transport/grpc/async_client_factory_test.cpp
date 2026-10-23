// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/scopeguard.h"

#include <tuple>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {
namespace {
using unittest::match::AllOf;
using unittest::match::Eq;
using unittest::match::Property;
using unittest::match::StatusIs;

MATCHER_P(SuccessfulStreamCount, successfulStreams, "") {
    return ExplainMatchResult(Property("successful streams",
                                       &GRPCConnectionStats::getTotalSuccessfulStreams,
                                       Eq(successfulStreams)),
                              arg,
                              result_listener);
}

MATCHER_P(FailedStreamCount, failedStreams, "") {
    return ExplainMatchResult(
        Property("failed streams", &GRPCConnectionStats::getTotalFailedStreams, Eq(failedStreams)),
        arg,
        result_listener);
}

MATCHER(HasNoRegisteredStreams, "") {
    return ExplainMatchResult(
        AllOf(Property("created streams", &GRPCConnectionStats::getTotalStreamsCreated, Eq(0)),
              Property("in-use streams", &GRPCConnectionStats::getTotalInUseStreams, Eq(0)),
              Property("leased streams", &GRPCConnectionStats::getTotalLeasedStreams, Eq(0))),
        arg,
        result_listener);
}

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
        const HostAndPort& target,
        Milliseconds timeout = CommandServiceTestFixtures::kDefaultConnectTimeout) {
        return getFactory().get(target, ConnectSSLMode::kGlobalSSLMode, timeout).get();
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
        auto resp = handle->getClient().runCommand_forTest(OpMsgRequest::parse(msg)).get();
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
                auto handle =
                    getClient(getTarget(), CommandServiceTestFixtures::kConcurrentConnectTimeout);
                for (int req = 0; req < 5; req++) {
                    auto msg = makeUniqueMessage();
                    auto resp =
                        handle->getClient().runCommand_forTest(OpMsgRequest::parse(msg)).get();
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
        ASSERT_EQ(handle1->getClient().runCommand_forTest(msg).getNoThrow(),
                  ErrorCodes::CallbackCanceled);
        ASSERT_EQ(handle2->getClient().runCommand_forTest(msg).getNoThrow(),
                  ErrorCodes::CallbackCanceled);

        // New sessions succeed and use a different underlying channel.
        auto handle3 = getClient();
        ON_BLOCK_EXIT([&] { handle3->indicateSuccess(); });

        ASSERT_NE(handle1ChannelId, getChannelIdForClient(handle3));

        ASSERT_OK(handle3->getClient().runCommand_forTest(msg).getNoThrow());
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
        ASSERT_EQ(handle1->getClient().runCommand_forTest(msg).getNoThrow(),
                  ErrorCodes::CallbackCanceled);

        // The other target is unaffected by dropConnections.
        ASSERT_OK(handle2->getClient().runCommand_forTest(msg).getNoThrow());
    }

    shutdownAndAssertOnTransportStats(1 /*successful streams*/, 1 /*failed streams*/);
}

TEST_F(GRPCAsyncClientFactoryTest, DropConnectionsWhileConnecting) {
    {
        // Start a stream establishment attempt that will hang until cancelled.
        auto connAttempt = getFactory().get(
            HostAndPort("localhost", 12345), ConnectSSLMode::kGlobalSSLMode, Milliseconds::max());
        assertStatsSoon(/*created=*/0, /*inUse=*/0, /*leased=*/0, /*open=*/1);
        getFactory().dropConnections();
        auto status = connAttempt.getNoThrow().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_THAT(status,
                    StatusIs(ErrorCodes::CallbackCanceled,
                             "Cancelled stream establishment due to dropping the associated "
                             "connection"));
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
        assertStatsSoon(/*created=*/0, /*inUse=*/0, /*leased=*/0, /*open=*/1);
        getFactory().dropConnections();

        // The first dropConnections does not affect the connect Future.
        ASSERT_FALSE(connAttempt.isReady());

        // Cancel the cancellation token to terminate the stream establishment.
        cancelSource.cancel();
        auto status = connAttempt.getNoThrow().getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_THAT(
            status,
            StatusIs(ErrorCodes::CallbackCanceled, "gRPC stream establishment was cancelled"));
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
        ASSERT_OK(handle1->getClient().runCommand_forTest(msg).getNoThrow());

        // New sessions are also unaffected.
        auto handle2 = getClient();
        ON_BLOCK_EXIT([&] { handle2->indicateSuccess(); });
        ASSERT_OK(handle2->getClient().runCommand_forTest(msg).getNoThrow());

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
        ASSERT_EQ(handle1->getClient().runCommand_forTest(msg).getNoThrow(),
                  ErrorCodes::CallbackCanceled);
        ASSERT_EQ(handle2->getClient().runCommand_forTest(msg).getNoThrow(),
                  ErrorCodes::CallbackCanceled);

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
        ASSERT_EQ(handle1->getClient().runCommand_forTest(msg).getNoThrow(),
                  ErrorCodes::CallbackCanceled);

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
    assertStatsSoon(/*created=*/3, /*inUse=*/2, /*leased=*/1, /*open=*/1);

    // Destroy two handles.
    handle->indicateSuccess();
    leasedHandle->indicateFailure({ErrorCodes::CallbackCanceled, "Destroying leased client"});
    handle.reset();
    leasedHandle.reset();

    // Handle outcome should not affect stats.
    assertStatsSoon(/*created=*/3, /*inUse=*/1, /*leased=*/0, /*open=*/1);

    auto msg = makeUniqueMessage();
    auto resp = anotherHandle->getClient().runCommand_forTest(OpMsgRequest::parse(msg)).get();
    ASSERT_OK(getStatusFromCommandResult(resp->getCommandReply()));

    // A handle is still in use after a command is run and before it is destroyed.
    assertStatsSoon(/*created=*/3, /*inUse=*/1, /*leased=*/0, /*open=*/1);

    anotherHandle->indicateSuccess();
    anotherHandle.reset();
    assertStatsSoon(/*created=*/3, /*inUse=*/0, /*leased=*/0, /*open=*/1);
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

/** Adds a reactor and client factory on top of MockGRPCTransportLayerTest. */
class DirectMockGRPCAsyncClientFactoryTest : public MockGRPCTransportLayerTest {
public:
    using Handle = std::shared_ptr<executor::AsyncClientFactory::AsyncClientHandle>;
    enum class Acquisition { kGet, kLease };
    enum class CancellationCause { kCaller, kConnectionDrop };

    void setUp() override {
        MockGRPCTransportLayerTest::setUp();
        _tl = checked_cast<GRPCTransportLayerMock*>(
            getServiceContext()->getTransportLayerManager()->getTransportLayer(
                TransportProtocol::GRPC));
        _reactor = _tl->getReactor(TransportLayer::WhichReactor::kNewReactor);
        _reactorThread = stdx::thread([this] {
            _reactor->run();
            _reactor->drain();
        });
        _factory = std::make_unique<GRPCAsyncClientFactory>("DirectMockGRPCAsyncClientFactoryTest");
        _factory->startup(getServiceContext(), _tl, _reactor);
    }

    void tearDown() override {
        if (_factory) {
            _factory->shutdown();
            _factory.reset();
        }
        _reactor->stop();
        _reactorThread.join();
        MockGRPCTransportLayerTest::tearDown();
    }

    GRPCAsyncClientFactory& getFactory() {
        return *_factory;
    }

    void destroyFactory() {
        _factory.reset();
    }

    const ReactorHandle& getReactor() {
        return _reactor;
    }

private:
    GRPCTransportLayerMock* _tl = nullptr;
    std::unique_ptr<GRPCAsyncClientFactory> _factory;
    ReactorHandle _reactor;
    stdx::thread _reactorThread;
};

class LateAcquisitionTest
    : public DirectMockGRPCAsyncClientFactoryTest,
      public testing::WithParamInterface<DirectMockGRPCAsyncClientFactoryTest::Acquisition> {};

TEST_P(LateAcquisitionTest, RejectAfterShutdown) {
    runTestWithMockServer([&] {
        boost::optional<FailPointEnableBlock> registration(
            "grpcAsyncClientFactoryHangBeforeHandleRegistration");
        boost::optional<FailPointEnableBlock> shutdownWait(
            "grpcAsyncClientFactoryHangBeforeShutdownWait");
        auto acquisition = makePromiseFuture<Handle>();
        auto acquisitionThread = stdx::thread([&] {
            acquisition.promise.setFrom(
                (GetParam() == Acquisition::kLease
                     ? getFactory().lease(
                           kServerHostAndPort, ConnectSSLMode::kGlobalSSLMode, Milliseconds::max())
                     : getFactory().get(
                           kServerHostAndPort, ConnectSSLMode::kGlobalSSLMode, Milliseconds::max()))
                    .getNoThrow());
        });
        stdx::thread shutdownThread;
        ON_BLOCK_EXIT([&] {
            shutdownWait.reset();
            registration.reset();
            acquisitionThread.join();
            if (shutdownThread.joinable()) {
                shutdownThread.join();
            }
        });
        registration->waitForOneNewEntry();
        shutdownThread = stdx::thread([&] { getFactory().shutdown(); });
        shutdownWait->waitForOneNewEntry();
        shutdownWait.reset();
        registration.reset();
        auto result = acquisition.future.getNoThrow();
        EXPECT_EQ(result.getStatus(), ErrorCodes::ShutdownInProgress);
        shutdownThread.join();
        EXPECT_THAT(
            getFactory().getStats(),
            AllOf(HasNoRegisteredStreams(), SuccessfulStreamCount(0), FailedStreamCount(1)));
    });
}

INSTANTIATE_TEST_SUITE_P(Acquisition,
                         LateAcquisitionTest,
                         testing::Values(DirectMockGRPCAsyncClientFactoryTest::Acquisition::kGet,
                                         DirectMockGRPCAsyncClientFactoryTest::Acquisition::kLease),
                         [](const auto& info) {
                             return info.param ==
                                     DirectMockGRPCAsyncClientFactoryTest::Acquisition::kGet
                                 ? "Get"
                                 : "Lease";
                         });

class PendingCancellationTest : public DirectMockGRPCAsyncClientFactoryTest,
                                public testing::WithParamInterface<std::string_view> {};

TEST_P(PendingCancellationTest, ShutdownCancelsIndefiniteEstablishment) {
    runTestWithMockServer([&] {
        FailPointEnableBlock pending(GetParam());
        auto acquisition = getFactory().get(kServerHostAndPort,
                                            ConnectSSLMode::kGlobalSSLMode,
                                            Milliseconds::max(),
                                            CancellationToken::uncancelable());
        pending.waitForOneNewEntry();
        getFactory().shutdown();
        ASSERT_EQ(acquisition.getNoThrow().getStatus(), ErrorCodes::ShutdownInProgress);
        EXPECT_THAT(
            getFactory().getStats(),
            AllOf(HasNoRegisteredStreams(), SuccessfulStreamCount(0), FailedStreamCount(0)));
    });
}

INSTANTIATE_TEST_SUITE_P(Establishment,
                         PendingCancellationTest,
                         testing::Values("grpcHangOnChannelEstablishment",
                                         "grpcHangOnStreamEstablishment"),
                         [](const auto& info) {
                             return info.param == "grpcHangOnChannelEstablishment" ? "Channel"
                                                                                   : "Stream";
                         });

class CancellationPreservationTest
    : public DirectMockGRPCAsyncClientFactoryTest,
      public testing::WithParamInterface<
          std::tuple<DirectMockGRPCAsyncClientFactoryTest::CancellationCause, std::string_view>> {};

TEST_P(CancellationPreservationTest, PreservesReasonDuringShutdown) {
    const auto& [cause, hook] = GetParam();
    runTestWithMockServer([&] {
        FailPointEnableBlock pending(hook);
        boost::optional<FailPointEnableBlock> shutdownWait(
            "grpcAsyncClientFactoryHangBeforeShutdownWait");
        CancellationSource caller;
        auto acquisition = getFactory().get(kServerHostAndPort,
                                            ConnectSSLMode::kGlobalSSLMode,
                                            Milliseconds::max(),
                                            caller.token());
        pending.waitForOneNewEntry();
        // entered and release ensure that the test remains lock-step with the reactor, in
        // particular it ensures that cancellation can't run on the reactor before shutdown has
        // been called.
        auto entered = makePromiseFuture<void>();
        auto release = makePromiseFuture<void>();
        getReactor()->schedule([&](Status) {
            entered.promise.emplaceValue();
            release.future.get();
        });
        stdx::thread shutdownThread;
        bool released = false;
        ON_BLOCK_EXIT([&] {
            shutdownWait.reset();
            if (!released) {
                release.promise.emplaceValue();
            }
            if (shutdownThread.joinable()) {
                shutdownThread.join();
            }
        });
        entered.future.get();
        if (cause == CancellationCause::kConnectionDrop) {
            static_cast<executor::AsyncClientFactory&>(getFactory()).dropConnections();
        } else {
            caller.cancel();
        }
        shutdownThread = stdx::thread([&] { getFactory().shutdown(); });
        shutdownWait->waitForOneNewEntry();
        shutdownWait.reset();
        release.promise.emplaceValue();
        released = true;
        auto status = acquisition.getNoThrow().getStatus();
        shutdownThread.join();
        EXPECT_THAT(
            status,
            StatusIs(
                ErrorCodes::CallbackCanceled,
                cause == CancellationCause::kConnectionDrop
                    ? "Cancelled stream establishment due to dropping the associated connection"
                    : "gRPC stream establishment was cancelled"));
        EXPECT_THAT(
            getFactory().getStats(),
            AllOf(HasNoRegisteredStreams(), SuccessfulStreamCount(0), FailedStreamCount(0)));
    });
}

INSTANTIATE_TEST_SUITE_P(
    Cancellation,
    CancellationPreservationTest,
    testing::Combine(
        testing::Values(DirectMockGRPCAsyncClientFactoryTest::CancellationCause::kCaller,
                        DirectMockGRPCAsyncClientFactoryTest::CancellationCause::kConnectionDrop),
        testing::Values("grpcHangOnStreamEstablishment", "grpcHangOnChannelEstablishment")),
    [](const auto& info) {
        const auto cause = std::get<0>(info.param);
        const auto hook = std::get<1>(info.param);
        return std::string(cause == DirectMockGRPCAsyncClientFactoryTest::CancellationCause::kCaller
                               ? "Caller"
                               : "ConnectionDrop") +
            (hook == "grpcHangOnStreamEstablishment" ? "DuringStreamEstablishment"
                                                     : "DuringChannelEstablishment");
    });

TEST_F(DirectMockGRPCAsyncClientFactoryTest, ShutdownWaitsForTimeoutDiagnostics) {
    unittest::ServerParameterGuard detailedLogging("enableDetailedConnectionHealthMetricLogLines",
                                                   true);
    runTestWithMockServer([&] {
        FailPointEnableBlock pending("grpcHangOnStreamEstablishment");
        boost::optional<FailPointEnableBlock> diagnostics(
            "grpcAsyncClientFactoryHangBeforeTimeoutDiagnostics");
        boost::optional<FailPointEnableBlock> shutdownWait(
            "grpcAsyncClientFactoryHangBeforeShutdownWait");
        auto acquisition =
            getFactory().get(kServerHostAndPort, ConnectSSLMode::kGlobalSSLMode, Milliseconds(1));
        auto shutdown = makePromiseFuture<void>();
        stdx::thread shutdownThread;
        ON_BLOCK_EXIT([&] {
            shutdownWait.reset();
            diagnostics.reset();
            if (shutdownThread.joinable()) {
                shutdownThread.join();
            }
        });
        diagnostics->waitForOneNewEntry();
        shutdownThread = stdx::thread([&] {
            getFactory().shutdown();
            shutdown.promise.emplaceValue();
        });
        shutdownWait->waitForOneNewEntry();
        shutdownWait.reset();
        auto client = getServiceContext()->getService()->makeClient("ShutdownWaitTest");
        // Creating an OperationContext with a deadline is the simplest way to ensure
        // shutdown.future get's a value within a reasonable amount of time.
        auto opCtx = client->makeOperationContext();
        opCtx->setDeadlineAfterNowBy(Milliseconds(500), ErrorCodes::ExceededTimeLimit);
        auto waitStatus = shutdown.future.getNoThrow(opCtx.get());
        diagnostics.reset();
        shutdownThread.join();
        auto acquisitionStatus = acquisition.getNoThrow().getStatus();
        EXPECT_THAT(getFactory().getStats(), AllOf(SuccessfulStreamCount(0), FailedStreamCount(0)));
        destroyFactory();
        EXPECT_EQ(waitStatus, ErrorCodes::ExceededTimeLimit);
        EXPECT_EQ(acquisitionStatus, ErrorCodes::ExceededTimeLimit);
    });
}

TEST_F(DirectMockGRPCAsyncClientFactoryTest, RejectAdmittedSetupBeforeConnect) {
    boost::optional<FailPointEnableBlock> beforeConnect("grpcAsyncClientFactoryHangBeforeConnect");
    boost::optional<FailPointEnableBlock> shutdownWait(
        "grpcAsyncClientFactoryHangBeforeShutdownWait");
    FailPointEnableBlock channel("grpcHangOnChannelEstablishment");
    auto acquisition = makePromiseFuture<Handle>();
    auto acquisitionThread = stdx::thread([&] {
        acquisition.promise.setFrom(
            getFactory()
                .get(kServerHostAndPort, ConnectSSLMode::kGlobalSSLMode, Milliseconds::max())
                .getNoThrow());
    });
    stdx::thread shutdownThread;
    ON_BLOCK_EXIT([&] {
        shutdownWait.reset();
        beforeConnect.reset();
        acquisitionThread.join();
        if (shutdownThread.joinable()) {
            shutdownThread.join();
        }
    });
    beforeConnect->waitForOneNewEntry();
    shutdownThread = stdx::thread([&] { getFactory().shutdown(); });
    shutdownWait->waitForOneNewEntry();
    shutdownWait.reset();
    beforeConnect.reset();
    EXPECT_EQ(acquisition.future.getNoThrow().getStatus(), ErrorCodes::ShutdownInProgress);
    shutdownThread.join();
    ASSERT_EQ(channel.waitForNNewEntries(0), 0);
    EXPECT_THAT(getFactory().getStats(),
                AllOf(HasNoRegisteredStreams(), SuccessfulStreamCount(0), FailedStreamCount(0)));
}

TEST_F(DirectMockGRPCAsyncClientFactoryTest, SynchronousStreamFactoryExceptionDrainsSetupTracking) {
    runTestWithMockServer([&] {
        FailPointEnableBlock failure("grpcThrowOnChannelEstablishment");
        auto acquisition = getFactory().get(
            kServerHostAndPort, ConnectSSLMode::kGlobalSSLMode, Milliseconds::max());
        failure.waitForOneNewEntry();
        auto status = acquisition.getNoThrow().getStatus();
        EXPECT_THAT(status,
                    StatusIs(ErrorCodes::HostUnreachable,
                             "throwing during channel establishment due to failpoint"));
        getFactory().shutdown();
        EXPECT_THAT(
            getFactory().getStats(),
            AllOf(HasNoRegisteredStreams(), SuccessfulStreamCount(0), FailedStreamCount(0)));
    });
}

TEST_F(DirectMockGRPCAsyncClientFactoryTest, ConnectionFailureDrainsSetupTracking) {
    runTestWithMockServer([&] {
        FailPointEnableBlock failure("grpcFailChannelEstablishment");
        auto result =
            getFactory()
                .get(kServerHostAndPort, ConnectSSLMode::kGlobalSSLMode, Milliseconds::max())
                .getNoThrow();
        ASSERT_EQ(result.getStatus(), ErrorCodes::HostUnreachable);
        getFactory().shutdown();
        EXPECT_THAT(
            getFactory().getStats(),
            AllOf(HasNoRegisteredStreams(), SuccessfulStreamCount(0), FailedStreamCount(0)));
    });
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

    fpb->waitForOneNewEntry();
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

        fpb->waitForOneNewEntry();
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
}  // namespace
}  // namespace mongo::transport::grpc
