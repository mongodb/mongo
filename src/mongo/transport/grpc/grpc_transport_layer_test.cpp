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

#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/grpc/wire_version_provider.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {
namespace {

class GRPCTransportLayerTest : public ServiceContextWithClockSourceMockTest {
public:
    void setUp() override {
        ServiceContextWithClockSourceMockTest::setUp();

        getServiceContext()->setPeriodicRunner(newPeriodicRunner());

        sslGlobalParams.sslCAFile = CommandServiceTestFixtures::kCAFile;
        sslGlobalParams.sslPEMKeyFile = CommandServiceTestFixtures::kServerCertificateKeyFile;
        sslGlobalParams.sslMode.store(SSLParams::SSLModes::SSLMode_requireSSL);
    }

    virtual std::unique_ptr<PeriodicRunner> newPeriodicRunner() {
        return makePeriodicRunner(getServiceContext());
    }

    static GRPCTransportLayer::Options makeTLOptions() {
        GRPCTransportLayer::Options options{};
        options.bindIpList = {};
        options.bindPort = CommandServiceTestFixtures::kBindPort;
        options.maxServerThreads = CommandServiceTestFixtures::kMaxThreads;
        options.useUnixDomainSockets = false;
        options.unixDomainSocketPermissions = DEFAULT_UNIX_PERMS;
        options.enableEgress = true;
        options.clientMetadata = makeClientMetadataDocument();

        return options;
    }

    std::unique_ptr<GRPCTransportLayer> makeTL(
        CommandService::RPCHandler serverCb = makeNoopRPCHandler(),
        GRPCTransportLayer::Options options = makeTLOptions()) {
        auto tl = std::make_unique<GRPCTransportLayer>(getServiceContext(), std::move(options));
        uassertStatusOK(tl->registerService(std::make_unique<CommandService>(
            tl.get(), std::move(serverCb), std::make_unique<WireVersionProvider>())));
        return tl;
    }

    static CommandService::RPCHandler makeNoopRPCHandler() {
        return [](auto session) {
            session->end();
        };
    }

    /**
     * Creates a GRPCTransportLayer using the provided RPCHandler and options, sets it up, starts
     * it, and then passes it to the provided callback, automatically shutting it down after the
     * callback completes.
     */
    void runWithTL(CommandService::RPCHandler serverCb,
                   std::function<void(GRPCTransportLayer&)> cb,
                   GRPCTransportLayer::Options options) {
        auto tl = makeTL(std::move(serverCb), std::move(options));
        uassertStatusOK(tl->setup());
        uassertStatusOK(tl->start());
        ON_BLOCK_EXIT([&] { tl->shutdown(); });
        cb(*tl);
    }

    void assertConnectSucceeds(GRPCTransportLayer& tl, const HostAndPort& addr) {
        auto session = makeEgressSession(tl, addr);
        ASSERT_OK(session->finish());
    }
};

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
        _tl = std::make_unique<GRPCTransportLayer>(getServiceContext(), makeTLOptions());
        uassertStatusOK(_tl->setup());
    }

    void tearDown() override {
        _tl.reset();
        ServiceContextWithClockSourceMockTest::tearDown();
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
        auto options = makeTLOptions();
        options.bindIpList = {"localhost", "127.0.0.1", "::1"};
        options.bindPort = CommandServiceTestFixtures::kBindPort;
        options.useUnixDomainSockets = true;

        std::vector<HostAndPort> addrs;
        for (auto& ip : options.bindIpList) {
            addrs.push_back(HostAndPort(ip, options.bindPort));
            addrs.push_back(HostAndPort(makeUnixSockPath(options.bindPort)));
        }

        runWithTL(
            makeNoopRPCHandler(),
            [&](auto& tl) {
                std::vector<stdx::thread> threads;
                for (size_t i = 0; i < addrs.size() * 5; i++) {
                    threads.push_back(monitor.spawn(
                        [&, i] { assertConnectSucceeds(tl, addrs[i % addrs.size()]); }));
                }

                for (auto& thread : threads) {
                    thread.join();
                }
            },
            std::move(options));
    });
}

TEST_F(GRPCTransportLayerTest, UnixDomainSocketPermissions) {
    auto options = makeTLOptions();
    auto permissions = S_IRWXO & S_IRWXG & S_IRWXU;
    options.useUnixDomainSockets = true;
    options.unixDomainSocketPermissions = permissions;

    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            struct stat st;
            ASSERT_EQ(::stat(makeUnixSockPath(options.bindPort).c_str(), &st), 0)
                << errorMessage(lastPosixError());
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

    std::vector<HostAndPort> addrs;
    addrs.push_back(HostAndPort("127.0.0.1", noIPListOptions.bindPort));
    addrs.push_back(HostAndPort(makeUnixSockPath(noIPListOptions.bindPort)));

    runWithTL(
        makeNoopRPCHandler(),
        [&](auto& tl) {
            for (auto& addr : addrs) {
                assertConnectSucceeds(tl, addr);
            }
        },
        std::move(noIPListOptions));
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
                ASSERT_TRUE(ErrorCodes::isNetworkError(status.getStatus()));
            };
            tryConnect();
            // Ensure second attempt on already created channel object also gracefully fails.
            tryConnect();
        },
        makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, GRPCTransportLayerShutdown) {
    auto tl = makeTL();
    auto client = std::make_shared<GRPCClient>(
        tl.get(), makeClientMetadataDocument(), CommandServiceTestFixtures::makeClientOptions());
    client->start(getServiceContext());
    ON_BLOCK_EXIT([&] { client->shutdown(); });

    {
        uassertStatusOK(tl->setup());
        uassertStatusOK(tl->start());
        ON_BLOCK_EXIT([&] { tl->shutdown(); });


        auto session = client->connect(CommandServiceTestFixtures::defaultServerAddress(),
                                       CommandServiceTestFixtures::kDefaultConnectTimeout,
                                       {});
        ASSERT_OK(session->finish());
        session.reset();
    }

    ASSERT_THROWS_CODE(
        client->connect(CommandServiceTestFixtures::defaultServerAddress(), Milliseconds(50), {}),
        DBException,
        ErrorCodes::NetworkTimeout);
    ASSERT_NOT_OK(tl->connect(CommandServiceTestFixtures::defaultServerAddress(),
                              ConnectSSLMode::kGlobalSSLMode,
                              Milliseconds(50)));
}

TEST_F(GRPCTransportLayerTest, Unary) {
    runWithTL(
        CommandServiceTestFixtures::makeEchoHandler(),
        [&](auto& tl) {
            auto session = makeEgressSession(tl);
            assertEchoSucceeds(*session);
            ASSERT_OK(session->finish());
        },
        makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, Exhaust) {
    constexpr auto kMessageCount = 5;

    auto streamingHandler = [](std::shared_ptr<IngressSession> session) {
        auto swMsg = session->sourceMessage();
        ASSERT_OK(swMsg);

        for (auto i = 0; i < kMessageCount; i++) {
            OpMsg response;
            response.body = BSON("i" << i);

            auto serialized = response.serialize();
            if (i < kMessageCount - 1) {
                OpMsg::setFlag(&serialized, OpMsg::kMoreToCome);
            }
            ASSERT_OK(session->sinkMessage(serialized));
        }
        session->end();
    };

    runWithTL(
        streamingHandler,
        [&](auto& tl) {
            auto session = makeEgressSession(tl);
            ASSERT_OK(session->sinkMessage(makeUniqueMessage()));
            for (auto i = 0;; i++) {
                auto swMsg = session->sourceMessage();
                ASSERT_OK(swMsg);

                auto responseMsg = OpMsg::parse(swMsg.getValue());
                int iReceived = responseMsg.body.getIntField("i");
                ASSERT_EQ(iReceived, i);

                if (!OpMsg::isFlagSet(swMsg.getValue(), OpMsg::kMoreToCome)) {
                    break;
                }
            }
            ASSERT_OK(session->finish());
        },
        makeTLOptions());
}

TEST_F(GRPCTransportLayerTest, Awaitable) {
    auto streamingHandler = [](std::shared_ptr<IngressSession> session) {
        auto swMsg = session->sourceMessage();
        ASSERT_OK(swMsg);

        for (auto i = 0;; i++) {
            OpMsg response;
            response.body = BSON("i" << i);
            auto serialized = response.serialize();
            OpMsg::setFlag(&serialized, OpMsg::kMoreToCome);

            try {
                uassertStatusOK(session->sinkMessage(serialized));
            } catch (ExceptionFor<ErrorCodes::StreamTerminated>&) {
                session->end();
                return;
            }

            sleepFor(Microseconds(500));
        }
    };

    constexpr auto kMessageCount = 5;
    runWithTL(
        streamingHandler,
        [&](auto& tl) {
            auto session = makeEgressSession(tl);
            ASSERT_OK(session->sinkMessage(makeUniqueMessage()));
            for (auto i = 0; i < kMessageCount; i++) {
                auto swMsg = session->sourceMessage();
                ASSERT_OK(swMsg);

                auto responseMsg = OpMsg::parse(swMsg.getValue());
                int iReceived = responseMsg.body.getIntField("i");
                ASSERT_EQ(iReceived, i);
            }
            session->end();
        },
        makeTLOptions());
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
            auto session = makeEgressSession(tl);
            assertEchoSucceeds(*session);

            auto unacknowledgedMsg = makeUniqueMessage();
            OpMsg::setFlag(&unacknowledgedMsg, OpMsg::kMoreToCome);
            ASSERT_OK(session->sinkMessage(unacknowledgedMsg));

            assertEchoSucceeds(*session);

            ASSERT_OK(session->finish());
        },
        makeTLOptions());
}

}  // namespace
}  // namespace mongo::transport::grpc
