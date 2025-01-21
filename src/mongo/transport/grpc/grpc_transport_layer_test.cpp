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

#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <vector>

#include <boost/filesystem.hpp>

#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc/client_cache.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/grpc_session_manager.h"
#include "mongo/transport/grpc/grpc_transport_layer_impl.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/grpc/wire_version_provider.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_workflow_test_util.h"
#include "mongo/transport/test_fixtures.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {
namespace {

class GRPCTransportLayerTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        auto svcCtx = getServiceContext();

        // Default SEP behavior is to fail.
        // Tests utilizing SEP workflows must provide an implementation
        // for serviceEntryPoint.handleRequestCb.
        auto sep = std::make_unique<MockServiceEntryPoint>();
        sep->handleRequestCb = [](OperationContext*, const Message&) -> Future<DbResponse> {
            MONGO_UNIMPLEMENTED;
        };
        serviceEntryPoint = sep.get();
        svcCtx->getService(ClusterRole::ShardServer)->setServiceEntryPoint(std::move(sep));
        svcCtx->setPeriodicRunner(newPeriodicRunner());
        ServiceExecutor::startupAll(svcCtx);

        sslGlobalParams.sslCAFile = CommandServiceTestFixtures::kCAFile;
        sslGlobalParams.sslPEMKeyFile = CommandServiceTestFixtures::kServerCertificateKeyFile;
        sslGlobalParams.sslMode.store(SSLParams::SSLModes::SSLMode_requireSSL);
    }

    void tearDown() override {
        ServiceContextTest::tearDown();
        ServiceExecutor::shutdownAll(getServiceContext(), Seconds{10});
    }

    virtual std::unique_ptr<PeriodicRunner> newPeriodicRunner() {
        return makePeriodicRunner(getServiceContext());
    }

    std::unique_ptr<GRPCTransportLayer> makeTL(
        CommandService::RPCHandler serverCb = makeNoopRPCHandler(),
        GRPCTransportLayer::Options options = CommandServiceTestFixtures::makeTLOptions()) {
        auto* svcCtx = getServiceContext();
        auto clientCache = std::make_shared<ClientCache>();
        std::vector<std::shared_ptr<ClientTransportObserver>> observers;
        auto sm = options.enableIngress
            ? std::make_unique<GRPCSessionManager>(svcCtx, clientCache, std::move(observers))
            : nullptr;
        auto tl =
            std::make_unique<GRPCTransportLayerImpl>(svcCtx, std::move(options), std::move(sm));

        if (options.enableIngress) {
            uassertStatusOK(tl->registerService(
                std::make_unique<CommandService>(tl.get(),
                                                 std::move(serverCb),
                                                 std::make_unique<WireVersionProvider>(),
                                                 std::move(clientCache))));
        }
        return tl;
    }

    static CommandService::RPCHandler makeNoopRPCHandler() {
        return [](auto session) {
            session->setTerminationStatus(Status::OK());
        };
    }

    static CommandService::RPCHandler makeActiveRPCHandler() {
        return [](auto session) {
            session->getTransportLayer()->getSessionManager()->startSession(std::move(session));
        };
    }

    /**
     * Creates a GRPCTransportLayer using the provided RPCHandler and options, sets it up, starts
     * it, and then passes it to the provided callback, automatically shutting it down after the
     * callback completes.
     *
     * The server handler will be run in a thread spawned from a ThreadAssertionMonitor to ensure
     * that test assertions fail the test. As a result, exceptions thrown by the handler will fail
     * the test, rather than being handled by CommandService.
     */
    void runWithTL(CommandService::RPCHandler serverCb,
                   std::function<void(GRPCTransportLayer&)> cb,
                   GRPCTransportLayer::Options options) {
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            auto tl = makeTL(
                [&](auto session) {
                    monitor.spawn([&] { ASSERT_DOES_NOT_THROW(serverCb(session)); }).join();
                },
                std::move(options));
            uassertStatusOK(tl->setup());
            uassertStatusOK(tl->start());
            ON_BLOCK_EXIT([&] { tl->shutdown(); });
            ASSERT_DOES_NOT_THROW(cb(*tl));
        });
    }

    void createAndStartupTL(bool ingress, bool egress) {
        auto options = CommandServiceTestFixtures::makeTLOptions();
        options.enableIngress = ingress;
        options.enableEgress = egress;

        auto tl = makeTL(makeNoopRPCHandler(), std::move(options));

        if (!ingress && !egress) {
            ASSERT_NOT_OK(tl->setup());
            ASSERT_NOT_OK(tl->start());
        } else {
            ASSERT_OK(tl->setup());
            ASSERT_OK(tl->start());
        }
        tl->shutdown();
    }

    void assertConnectSucceeds(GRPCTransportLayer& tl, const HostAndPort& addr) {
        auto session = makeEgressSession(tl, addr);
        ASSERT_OK(session->finish());
    }

    static Message makeMessage(BSONObj body) {
        OpMsgBuilder builder;
        builder.setBody(body);
        return builder.finish();
    }

    static BSONObj getMessageBody(const Message& message) {
        return OpMsg::parse(message).body.getOwned();
    }

    std::shared_ptr<GRPCReactor> getGRPCEgressReactor(TransportLayer* tl) {
        return std::dynamic_pointer_cast<GRPCReactor>(
            tl->getReactor(TransportLayer::WhichReactor::kEgress));
    }

    /**
     * Exercises the session manager and service entry point codepath through sending the specified
     * message from the client to the server, which responds back to the client with the same
     * message.
     */
    void runCommandThroughServiceEntryPoint(StringData message) {
        constexpr auto kCommandName = "mockCommand"_sd;
        constexpr auto kReplyField = "mockReply"_sd;
        serviceEntryPoint->handleRequestCb = [&](OperationContext*,
                                                 const Message& request) -> Future<DbResponse> {
            ASSERT_EQ(OpMsg::parse(request).body.firstElement().fieldName(), kCommandName);
            OpMsgBuilder reply;
            reply.setBody(BSON(kReplyField << message));
            return DbResponse{.response = reply.finish()};
        };

        auto cb = [&](GRPCTransportLayer& tl) {
            auto client =
                std::make_shared<GRPCClient>(&tl,
                                             getServiceContext(),
                                             makeClientMetadataDocument(),
                                             CommandServiceTestFixtures::makeClientOptions());
            client->start();
            ON_BLOCK_EXIT([&] { client->shutdown(); });

            auto session = client
                               ->connect(tl.getListeningAddresses().at(0),
                                         getGRPCEgressReactor(&tl),
                                         CommandServiceTestFixtures::kDefaultConnectTimeout,
                                         {})
                               .get();

            ASSERT_OK(session->sinkMessage(makeMessage(BSON(kCommandName << message))));
            auto replyMessage = uassertStatusOK(session->sourceMessage());
            auto replyBody = getMessageBody(replyMessage);
            ASSERT_EQ(replyBody.firstElement().fieldName(), kReplyField);

            ASSERT_OK(session->finish());
        };

        runWithTL(makeActiveRPCHandler(), cb, CommandServiceTestFixtures::makeTLOptions());
    }

    MockServiceEntryPoint* serviceEntryPoint;
    test::SSLGlobalParamsGuard _sslGlobalParamsGuard;
    unittest::MinimumLoggedSeverityGuard logSeverityGuardNetwork{logv2::LogComponent::kNetwork,
                                                                 logv2::LogSeverity::Debug(3)};
};

TEST_F(GRPCTransportLayerTest, startupIngressAndEgress) {
    createAndStartupTL(true, true);
}

TEST_F(GRPCTransportLayerTest, startupIngressNoEgress) {
    createAndStartupTL(true, false);
}

TEST_F(GRPCTransportLayerTest, startupEgressNoIngress) {
    createAndStartupTL(false, true);
}

TEST_F(GRPCTransportLayerTest, startupNeitherIngressNorEgress) {
    createAndStartupTL(false, false);
}

TEST_F(GRPCTransportLayerTest, startupEgressWithoutTLS) {
    sslGlobalParams.sslCAFile.clear();
    sslGlobalParams.sslPEMKeyFile.clear();
    createAndStartupTL(false, true);
}

TEST_F(GRPCTransportLayerTest, setupIngressWithoutTLSShouldFail) {
    sslGlobalParams.sslCAFile.clear();
    sslGlobalParams.sslPEMKeyFile.clear();

    auto options = CommandServiceTestFixtures::makeTLOptions();
    options.enableIngress = true;

    auto tl = makeTL(makeNoopRPCHandler(), std::move(options));
    ASSERT_EQ(ErrorCodes::InvalidOptions, tl->setup());
}


TEST_F(GRPCTransportLayerTest, RunCommand) {
    runCommandThroughServiceEntryPoint("x");
}

TEST_F(GRPCTransportLayerTest, RunLargeCommand) {
    std::string largeMessage(5 * 1024 * 1024, 'x');
    runCommandThroughServiceEntryPoint(largeMessage);
}

TEST_F(GRPCTransportLayerTest, TransportLayerStartsEgressReactor) {
    runWithTL(
        makeNoopRPCHandler(),
        [](GRPCTransportLayer& tl) {
            auto reactor = tl.getReactor(TransportLayer::WhichReactor::kEgress);

            // Schedule a single task on the reactor to make sure it is working.
            auto pf = makePromiseFuture<void>();
            reactor->schedule([&](Status status) { pf.promise.setFrom(status); });
            ASSERT_OK(std::move(pf.future).getNoThrow());
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, StopAfterSetup) {
    auto options = CommandServiceTestFixtures::makeTLOptions();
    options.enableEgress = true;
    options.enableIngress = true;

    auto tl =
        std::make_unique<GRPCTransportLayerImpl>(getServiceContext(), std::move(options), nullptr);
    ASSERT_OK(tl->setup());
    tl->shutdown();
}

/**
 * Modifies the `ServiceContext` with `PeriodicRunnerMock`, a custom `PeriodicRunner` that maintains
 * a list of all instances of `PeriodicJob` and allows monitoring their internal state. We use this
 * modified runner to test proper initialization and teardown of the idle channel pruner.
 */
class IdleChannelPrunerTest : public GRPCTransportLayerTest {
public:
    class PeriodicRunnerMock : public PeriodicRunner {
    public:
        /**
         * Owns and monitors a `PeriodicJob` by maintaining its observable state (e.g., `kPause`).
         */
        class ControllableJobMock : public ControllableJob {
        public:
            enum class State { kNotSet, kStart, kPause, kResume, kStop };

            explicit ControllableJobMock(PeriodicJob job) : _job(std::move(job)) {}

            void start() override {
                _setState(State::kStart);
            }

            void pause() override {
                _setState(State::kPause);
            }

            void resume() override {
                _setState(State::kResume);
            }

            void stop() override {
                _setState(State::kStop);
            }

            Milliseconds getPeriod() const override {
                return _job.interval;
            }

            void setPeriod(Milliseconds ms) override {
                _job.interval = ms;
            }

            bool isStarted() const {
                return _state == State::kStart;
            }

            bool isStopped() const {
                return _state == State::kStop;
            }

        private:
            void _setState(State newState) {
                LOGV2(7401901,
                      "Updating state for a `PeriodicJob`",
                      "jobName"_attr = _job.name,
                      "oldState"_attr = _state,
                      "newState"_attr = newState);
                _state = newState;
            }

            State _state = State::kNotSet;
            PeriodicJob _job;
        };

        PeriodicJobAnchor makeJob(PeriodicJob job) override {
            auto handle = std::make_shared<ControllableJobMock>(std::move(job));
            jobs.push_back(handle);
            return PeriodicJobAnchor{std::move(handle)};
        }

        std::vector<std::shared_ptr<ControllableJobMock>> jobs;
    };

    void setUp() override {
        GRPCTransportLayerTest::setUp();
        auto* svcCtx = getServiceContext();
        std::vector<std::shared_ptr<ClientTransportObserver>> observers;
        auto sm = std::make_unique<GRPCSessionManager>(
            svcCtx, std::make_shared<ClientCache>(), std::move(observers));
        _tl = std::make_unique<GRPCTransportLayerImpl>(
            getServiceContext(), CommandServiceTestFixtures::makeTLOptions(), std::move(sm));
        uassertStatusOK(_tl->setup());
    }

    void tearDown() override {
        _tl.reset();
        ServiceContextTest::tearDown();
    }

    GRPCTransportLayer& transportLayer() {
        return *_tl;
    }

    std::unique_ptr<PeriodicRunner> newPeriodicRunner() override {
        return std::make_unique<PeriodicRunnerMock>();
    }

    PeriodicRunnerMock* getPeriodicRunnerMock() {
        return static_cast<PeriodicRunnerMock*>(getServiceContext()->getPeriodicRunner());
    }

private:
    std::unique_ptr<GRPCTransportLayer> _tl;
};

TEST_F(IdleChannelPrunerTest, StartsWithTransportLayer) {
    ASSERT_TRUE(getPeriodicRunnerMock()->jobs.empty());
    ASSERT_OK(transportLayer().start());
    ON_BLOCK_EXIT([&] { transportLayer().shutdown(); });
    ASSERT_EQ(getPeriodicRunnerMock()->jobs.size(), 1);
    auto& prunerJob = getPeriodicRunnerMock()->jobs[0];
    ASSERT_EQ(prunerJob->getPeriod(), Client::kDefaultChannelTimeout);
    ASSERT_TRUE(prunerJob->isStarted());
}

TEST_F(IdleChannelPrunerTest, StopsWithTransportLayer) {
    ASSERT_OK(transportLayer().start());
    transportLayer().shutdown();
    ASSERT_EQ(getPeriodicRunnerMock()->jobs.size(), 1);
    auto& prunerJob = getPeriodicRunnerMock()->jobs[0];
    ASSERT_TRUE(prunerJob->isStopped());
}

TEST_F(GRPCTransportLayerTest, ConnectAndListen) {
    unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
        auto options = CommandServiceTestFixtures::makeTLOptions();
        options.bindIpList = {"localhost", "127.0.0.1", "::1"};
        options.useUnixDomainSockets = true;

        runWithTL(
            makeNoopRPCHandler(),
            [&](auto& tl) {
                auto addrs = tl.getListeningAddresses();
                std::vector<stdx::thread> threads;
                for (size_t i = 0; i < addrs.size() * 5; i++) {
                    threads.push_back(monitor.spawn([&, i] {
                        ASSERT_DOES_NOT_THROW(assertConnectSucceeds(tl, addrs[i % addrs.size()]));
                    }));
                }

                for (auto& thread : threads) {
                    thread.join();
                }
            },
            std::move(options));
    });
}

TEST_F(GRPCTransportLayerTest, UnixDomainSocketPermissions) {
    auto options = CommandServiceTestFixtures::makeTLOptions();
    auto permissions = S_IRWXO & S_IRWXG & S_IRWXU;
    options.useUnixDomainSockets = true;
    options.unixDomainSocketPermissions = permissions;

    runWithTL(
        makeNoopRPCHandler(),
        [&](GRPCTransportLayer& tl) {
            auto addrs = tl.getListeningAddresses();
            auto socketPath = std::find_if(addrs.begin(), addrs.end(), [](const HostAndPort& hp) {
                return isUnixDomainSocket(hp.host());
            });
            ASSERT_NE(socketPath, addrs.end());

            struct stat st;
            ASSERT_EQ(::stat(socketPath->host().c_str(), &st), 0) << errorMessage(lastPosixError());
            ASSERT_EQ(st.st_mode & permissions, permissions);
        },
        std::move(options));
}

// Verifies that when provided an empty list of binding addresses, the TL starts a server that
// listens in all the required places, including Unix domain sockets.
TEST_F(GRPCTransportLayerTest, DefaultIPList) {
    GRPCTransportLayer::Options noIPListOptions;
    noIPListOptions.enableEgress = true;
    noIPListOptions.clientMetadata = makeClientMetadataDocument();
    noIPListOptions.bindIpList = {};
    noIPListOptions.useUnixDomainSockets = true;
    noIPListOptions.bindPort = 0;

    runWithTL(
        makeNoopRPCHandler(),
        [&](GRPCTransportLayer& tl) {
            for (auto& addr : tl.getListeningAddresses()) {
                ASSERT(addr.isLocalHost() || isUnixDomainSocket(addr.host()))
                    << "unexpected default address: " << addr;
                assertConnectSucceeds(tl, addr);
            }
        },
        std::move(noIPListOptions));
}

TEST_F(GRPCTransportLayerTest, ConcurrentAsyncConnectsSucceed) {
    runWithTL(
        CommandServiceTestFixtures::makeEchoHandler(),
        [&](auto& tl) {
            const size_t kNumConcurrentStreamEstablishments = 20;
            std::vector<Future<std::shared_ptr<Session>>> streamFutures;

            for (size_t i = 0; i < kNumConcurrentStreamEstablishments; i++) {
                streamFutures.push_back(
                    tl.asyncConnect(tl.getListeningAddresses().at(0),
                                    ConnectSSLMode::kGlobalSSLMode,
                                    tl.getReactor(TransportLayer::WhichReactor::kEgress),
                                    CommandServiceTestFixtures::kDefaultConnectTimeout,
                                    nullptr /** connectionMetrics */,
                                    nullptr /** transientSSLContext */));
            }

            for (auto& fut : streamFutures) {
                auto swSession = fut.getNoThrow();
                ASSERT_OK(swSession.getStatus());

                // Ensure the sessions are useable.
                std::shared_ptr<EgressSession> session =
                    checked_pointer_cast<EgressSession>(swSession.getValue());
                assertEchoSucceeds(*session);
                ASSERT_OK(session->finish());
            }
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, AsyncConnectConnectionMetrics) {
    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            auto connMetrics =
                std::make_shared<ConnectionMetrics>(getServiceContext()->getFastClockSource());
            auto fut = tl.asyncConnect(tl.getListeningAddresses().at(0),
                                       ConnectSSLMode::kGlobalSSLMode,
                                       tl.getReactor(TransportLayer::WhichReactor::kEgress),
                                       CommandServiceTestFixtures::kDefaultConnectTimeout,
                                       connMetrics,
                                       nullptr /** transientSSLContext */);
            auto swSession = fut.getNoThrow();
            ASSERT_OK(swSession.getStatus());

            ASSERT_GT(connMetrics->dnsResolution().get(), Milliseconds(0));
            // These timers are resolved immediately after the dnsResolution timer.
            ASSERT_GTE(connMetrics->tcpConnection().get(), Milliseconds(0));
            ASSERT_GTE(connMetrics->tlsHandshake().get(), Milliseconds(0));
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, ConnectionError) {
    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            auto tryConnect = [&] {
                auto status = tl.connect(HostAndPort("localhost", 1235),
                                         ConnectSSLMode::kGlobalSSLMode,
                                         Milliseconds(50));
                ASSERT_NOT_OK(status);
                ASSERT_EQ(status.getStatus().code(), ErrorCodes::HostUnreachable);
            };
            tryConnect();
            // Ensure second attempt on already created channel object also gracefully fails.
            tryConnect();
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, SSLModeMismatch) {
    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            auto tryConnect = [&] {
                auto status = tl.connect(tl.getListeningAddresses().at(0),
                                         ConnectSSLMode::kDisableSSL,
                                         CommandServiceTestFixtures::kDefaultConnectTimeout);
                ASSERT_NOT_OK(status);
                ASSERT_EQ(status.getStatus().code(), ErrorCodes::HostUnreachable);
            };
            tryConnect();
            // Ensure second attempt on already created channel object also gracefully fails.
            tryConnect();
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, GRPCTransportLayerShutdown) {
    auto tl = makeTL();
    auto client = std::make_shared<GRPCClient>(tl.get(),
                                               getServiceContext(),
                                               makeClientMetadataDocument(),
                                               CommandServiceTestFixtures::makeClientOptions());
    client->start();
    ON_BLOCK_EXIT([&] { client->shutdown(); });

    HostAndPort addr;
    {
        uassertStatusOK(tl->setup());
        uassertStatusOK(tl->start());
        ON_BLOCK_EXIT([&] { tl->shutdown(); });
        addr = tl->getListeningAddresses().at(0);

        auto session = client
                           ->connect(addr,
                                     getGRPCEgressReactor(tl.get()),
                                     CommandServiceTestFixtures::kDefaultConnectTimeout,
                                     {})
                           .get();
        ASSERT_OK(session->finish());
        session.reset();
    }

    ASSERT_THROWS_CODE(
        client->connect(addr, getGRPCEgressReactor(tl.get()), Milliseconds(50), {}).get(),
        DBException,
        ErrorCodes::ShutdownInProgress);
    ASSERT_NOT_OK(tl->connect(addr, ConnectSSLMode::kGlobalSSLMode, Milliseconds(50)));
}

TEST_F(GRPCTransportLayerTest, TryCancelAfterReactorShutdown) {
    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            auto session = makeEgressSession(tl, tl.getListeningAddresses().at(0));
            getGRPCEgressReactor(&tl)->stop();
            ASSERT_DOES_NOT_THROW(session->cancel(Status(ErrorCodes::CallbackCanceled, "test")));
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, Unary) {
    runWithTL(
        CommandServiceTestFixtures::makeEchoHandler(),
        [&](auto& tl) {
            auto session = makeEgressSession(tl, tl.getListeningAddresses().at(0));
            assertEchoSucceeds(*session);
            ASSERT_OK(session->finish());
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, Exhaust) {
    // In this test, the client side stops reading after kMessageCount exhaust replies and then
    // cancels the RPC. The server will verify that it was able to successfully transmit at least
    // kMessageCount messages before it observed the RPC being cancelled.
    constexpr auto kMessageCount = 5;

    Notification<void> serverHandlerDone;

    auto streamingHandler = [&](std::shared_ptr<IngressSession> session) {
        ON_BLOCK_EXIT([&] { serverHandlerDone.set(); });
        auto swMsg = session->sourceMessage();
        ASSERT_OK(swMsg);

        for (auto i = 0;; i++) {
            OpMsg response;
            response.body = BSON("i" << i);

            auto serialized = response.serialize();
            OpMsg::setFlag(&serialized, OpMsg::kMoreToCome);

            if (auto sinkStatus = session->sinkMessage(serialized); !sinkStatus.isOK()) {
                ASSERT_EQ(sinkStatus.code(), ErrorCodes::CallbackCanceled);
                ASSERT_GTE(i, kMessageCount);
                break;
            }

            sleepFor(Microseconds(500));
        }
        ASSERT_FALSE(session->isConnected());
        ASSERT_EQ(session->terminationStatus()->code(), ErrorCodes::CallbackCanceled);
    };

    runWithTL(
        streamingHandler,
        [&](auto& tl) {
            auto session = makeEgressSession(tl, tl.getListeningAddresses().at(0));
            ASSERT_OK(session->sinkMessage(makeUniqueMessage()));
            for (auto i = 0; i < kMessageCount; i++) {
                auto swMsg = session->sourceMessage();
                ASSERT_OK(swMsg);

                auto responseMsg = OpMsg::parse(swMsg.getValue());
                int iReceived = responseMsg.body.getIntField("i");
                ASSERT_EQ(iReceived, i);
            }
            session->end();

            // Wait here before exiting to ensure that server handler has a chance to receive the
            // cancellation.
            serverHandlerDone.get();
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, Unacknowledged) {
    auto serverHandler = [](std::shared_ptr<IngressSession> session) {
        while (true) {
            try {
                auto swMsg = session->sourceMessage();
                uassertStatusOK(swMsg);
                if (OpMsg::isFlagSet(swMsg.getValue(), OpMsg::kMoreToCome)) {
                    continue;
                }
                ASSERT_OK(session->sinkMessage(swMsg.getValue()));
            } catch (ExceptionFor<ErrorCodes::StreamTerminated>&) {
                session->end();
                return;
            }
        }
    };

    runWithTL(
        serverHandler,
        [&](auto& tl) {
            auto session = makeEgressSession(tl, tl.getListeningAddresses().at(0));
            assertEchoSucceeds(*session);

            auto unacknowledgedMsg = makeUniqueMessage();
            OpMsg::setFlag(&unacknowledgedMsg, OpMsg::kMoreToCome);
            ASSERT_OK(session->sinkMessage(unacknowledgedMsg));

            assertEchoSucceeds(*session);

            ASSERT_OK(session->finish());
        },
        CommandServiceTestFixtures::makeTLOptions());
}

class RotateCertificatesGRPCTransportLayerTest : public GRPCTransportLayerTest {
public:
    void setUp() override {
        GRPCTransportLayerTest::setUp();
        _tempDir =
            test::copyCertsToTempDir(grpc::CommandServiceTestFixtures::kCAFile,
                                     grpc::CommandServiceTestFixtures::kServerCertificateKeyFile,
                                     grpc::CommandServiceTestFixtures::kClientCertificateKeyFile,
                                     "grpc");

        sslGlobalParams.sslCAFile = _tempDir->getCAFile().toString();
        sslGlobalParams.sslPEMKeyFile = _tempDir->getPEMKeyFile().toString();
    }

    const test::TempCertificatesDir& getCertificatesDir() const {
        return *_tempDir;
    }

    std::shared_ptr<GRPCClient> startClient() {
        GRPCClient::Options options{};
        options.tlsCAFile = sslGlobalParams.sslCAFile;
        options.tlsCertificateKeyFile = getCertificatesDir().getClientPEMKeyFile();
        auto client = std::make_shared<GRPCClient>(nullptr /* transport layer */,
                                                   getServiceContext(),
                                                   makeClientMetadataDocument(),
                                                   std::move(options));
        client->start();
        return client;
    }

private:
    std::unique_ptr<test::TempCertificatesDir> _tempDir;
};

TEST_F(RotateCertificatesGRPCTransportLayerTest, RotateCertificatesSucceeds) {
    // Ceritificates that we wil rotate to.
    const std::string kTrustedCAFile = "jstests/libs/trusted-ca.pem";
    const std::string kTrustedPEMFile = "jstests/libs/trusted-server.pem";
    const std::string kTrustedClientFile = "jstests/libs/trusted-client.pem";

    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            auto addr = tl.getListeningAddresses().at(0);
            auto initialGoodStub = CommandServiceTestFixtures::makeStubWithCerts(
                addr,
                CommandServiceTestFixtures::kCAFile,
                CommandServiceTestFixtures::kClientCertificateKeyFile);
            auto initialBadStub = CommandServiceTestFixtures::makeStubWithCerts(
                addr, kTrustedCAFile, kTrustedClientFile);

            ASSERT_GRPC_STUB_CONNECTED(initialGoodStub);
            ASSERT_GRPC_STUB_NOT_CONNECTED(initialBadStub);

            // Overwrite the tmp files to hold new certs.
            boost::filesystem::copy_file(kTrustedCAFile,
                                         getCertificatesDir().getCAFile().toString(),
                                         boost::filesystem::copy_options::overwrite_existing);
            boost::filesystem::copy_file(kTrustedPEMFile,
                                         getCertificatesDir().getPEMKeyFile().toString(),
                                         boost::filesystem::copy_options::overwrite_existing);

            ASSERT_OK(tl.rotateCertificates(SSLManagerCoordinator::get()->getSSLManager(), false));

            ASSERT_GRPC_STUB_CONNECTED(initialGoodStub);
            ASSERT_GRPC_STUB_CONNECTED(initialBadStub);
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(RotateCertificatesGRPCTransportLayerTest, RotateCertificatesSucceedsWhenUnchanged) {
    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            auto addr = tl.getListeningAddresses().at(0);
            // Connect using the existing certs.
            auto stub = CommandServiceTestFixtures::makeStubWithCerts(
                addr,
                CommandServiceTestFixtures::kCAFile,
                CommandServiceTestFixtures::kClientCertificateKeyFile);
            ASSERT_GRPC_STUB_CONNECTED(stub);

            ASSERT_OK(tl.rotateCertificates(SSLManagerCoordinator::get()->getSSLManager(), false));

            auto stub2 = CommandServiceTestFixtures::makeStubWithCerts(
                addr,
                CommandServiceTestFixtures::kCAFile,
                CommandServiceTestFixtures::kClientCertificateKeyFile);
            ASSERT_GRPC_STUB_CONNECTED(stub2);
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(RotateCertificatesGRPCTransportLayerTest, RotateCertificatesThrowsAndUsesOldCertsWhenEmpty) {
    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            auto addr = tl.getListeningAddresses().at(0);

            // Connect using the existing certs.
            auto stub = CommandServiceTestFixtures::makeStubWithCerts(
                addr,
                CommandServiceTestFixtures::kCAFile,
                CommandServiceTestFixtures::kClientCertificateKeyFile);
            ASSERT_GRPC_STUB_CONNECTED(stub);

            boost::filesystem::resize_file(getCertificatesDir().getCAFile().toString(), 0);

            ASSERT_EQ(
                tl.rotateCertificates(SSLManagerCoordinator::get()->getSSLManager(), false).code(),
                ErrorCodes::InvalidSSLConfiguration);

            auto stub2 = CommandServiceTestFixtures::makeStubWithCerts(
                addr,
                CommandServiceTestFixtures::kCAFile,
                CommandServiceTestFixtures::kClientCertificateKeyFile);
            ASSERT_GRPC_STUB_CONNECTED(stub2);
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(RotateCertificatesGRPCTransportLayerTest,
       RotateCertificatesUsesOldCertsWithNewInvalidCerts) {
    const std::string kInvalidPEMFile = "jstests/libs/ecdsa-ca-ocsp.crt";

    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            auto addr = tl.getListeningAddresses().at(0);
            auto client = startClient();
            auto reactor = checked_pointer_cast<GRPCReactor>(
                tl.getReactor(GRPCTransportLayer::WhichReactor::kEgress));

            // Connect using the existing certs.
            auto stub = CommandServiceTestFixtures::makeStubWithCerts(
                addr,
                CommandServiceTestFixtures::kCAFile,
                CommandServiceTestFixtures::kClientCertificateKeyFile);
            ASSERT_GRPC_STUB_CONNECTED(stub);

            ASSERT_OK(client->connect(addr, reactor, Milliseconds(500), {}).getNoThrow());
            client->dropAllChannels_forTest();

            // Overwrite the tmp files to hold new, invalid certs.
            boost::filesystem::copy_file(kInvalidPEMFile,
                                         getCertificatesDir().getPEMKeyFile().toString(),
                                         boost::filesystem::copy_options::overwrite_existing);
            boost::filesystem::copy_file(kInvalidPEMFile,
                                         getCertificatesDir().getClientPEMKeyFile().toString(),
                                         boost::filesystem::copy_options::overwrite_existing);

            ASSERT_EQ(
                tl.rotateCertificates(SSLManagerCoordinator::get()->getSSLManager(), false).code(),
                ErrorCodes::InvalidSSLConfiguration);

            SSLConfiguration newConfig{};
            newConfig.serverCertificateExpirationDate =
                Date_t::fromDurationSinceEpoch(Milliseconds(1234));
            ASSERT_EQ(client->rotateCertificates(newConfig), ErrorCodes::InvalidSSLConfiguration);

            // Make sure we can still connect with the initial certs used before the bad
            // rotation.
            auto stub2 = CommandServiceTestFixtures::makeStubWithCerts(
                addr,
                CommandServiceTestFixtures::kCAFile,
                CommandServiceTestFixtures::kClientCertificateKeyFile);
            ASSERT_GRPC_STUB_CONNECTED(stub2);

            // The already-existing client should also also still be using the old certs after a
            // failed rotation.
            auto swSession = client->connect(addr, reactor, Milliseconds(500), {}).getNoThrow();
            ASSERT_OK(swSession);

            // SSLConfiguration should remain unchanged.
            ASSERT_NE(swSession.getValue()->getSSLConfiguration()->serverCertificateExpirationDate,
                      newConfig.serverCertificateExpirationDate);
        },
        CommandServiceTestFixtures::makeTLOptions());
}

TEST_F(RotateCertificatesGRPCTransportLayerTest, ClientUsesOldCertsUntilRotate) {
    // Ceritificates that we wil rotate to.
    const std::string kTrustedCAFile = "jstests/libs/trusted-ca.pem";
    const std::string kTrustedPEMFile = "jstests/libs/trusted-server.pem";
    const std::string kTrustedClientPEMFile = "jstests/libs/trusted-client.pem";

    runWithTL(
        makeNoopRPCHandler(),
        [&](GRPCTransportLayer& tl) {
            auto addr = tl.getListeningAddresses().at(0);

            auto reactor = checked_pointer_cast<GRPCReactor>(
                tl.getReactor(GRPCTransportLayer::WhichReactor::kEgress));

            auto client = startClient();

            // First connect should succeed.
            ASSERT_OK(
                client
                    ->connect(addr, reactor, CommandServiceTestFixtures::kDefaultConnectTimeout, {})
                    .getNoThrow());

            // Overwrite the tmp files to hold new certs.
            boost::filesystem::copy_file(kTrustedCAFile,
                                         getCertificatesDir().getCAFile().toString(),
                                         boost::filesystem::copy_options::overwrite_existing);
            boost::filesystem::copy_file(kTrustedPEMFile,
                                         getCertificatesDir().getPEMKeyFile().toString(),
                                         boost::filesystem::copy_options::overwrite_existing);
            boost::filesystem::copy_file(kTrustedClientPEMFile,
                                         getCertificatesDir().getClientPEMKeyFile().toString(),
                                         boost::filesystem::copy_options::overwrite_existing);
            // Rotate the certificates server side.
            ASSERT_OK(tl.rotateCertificates(SSLManagerCoordinator::get()->getSSLManager(), false));

            // After rotating on the server side, connecting from the client should fail, as it is
            // still using the old certificates.
            client->dropAllChannels_forTest();
            ASSERT_NOT_OK(
                client
                    ->connect(addr, reactor, CommandServiceTestFixtures::kDefaultConnectTimeout, {})
                    .getNoThrow());

            // After rotation, connection should now succeed.
            client->dropAllChannels_forTest();

            SSLConfiguration newConfig{};
            newConfig.serverCertificateExpirationDate = Date_t::fromMillisSinceEpoch(1234);
            ASSERT_OK(client->rotateCertificates(newConfig));
            auto swSession =
                client
                    ->connect(addr, reactor, CommandServiceTestFixtures::kDefaultConnectTimeout, {})
                    .getNoThrow();
            ASSERT_OK(swSession);
            ASSERT_EQ(swSession.getValue()->getSSLConfiguration()->serverCertificateExpirationDate,
                      newConfig.serverCertificateExpirationDate);
        },
        CommandServiceTestFixtures::makeTLOptions());
}

}  // namespace
}  // namespace mongo::transport::grpc
