// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/mock_client.h"
#include "mongo/transport/grpc/reactor.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::transport {
namespace [[MONGO_MOD_PARENT_PRIVATE]] grpc {

class Service;

/**
 * Currently only mocks the egress portion of GRPCTransportLayer.
 *
 * setup() must be called exactly once before start(), which also can only be called exactly once.
 * Neither of these methods are thread-safe.
 */
class GRPCTransportLayerMock : public GRPCTransportLayer {
public:
    GRPCTransportLayerMock(ServiceContext* svcCtx,
                           Options options,
                           MockClient::MockResolver resolver,
                           const HostAndPort& mockClientAddress);

    Status registerService(std::unique_ptr<Service> svc) override;

    Status setup() override;

    Status start() override;

    void shutdown() override;

    void stopAcceptingSessions() override {
        MONGO_UNIMPLEMENTED;
    }

    std::shared_ptr<Client> createGRPCClient(
        BSONObj clientMetadata = makeClientMetadataDocument()) override;

    StatusWith<std::shared_ptr<Session>> connectWithAuthToken(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        boost::optional<std::string> authToken = boost::none) override;

    StatusWith<std::shared_ptr<Session>> connect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        const boost::optional<TransientSSLParams>& transientSSLParams) override;

    Future<std::shared_ptr<Session>> asyncConnectWithAuthToken(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        const CancellationToken& token = CancellationToken::uncancelable(),
        boost::optional<std::string> authToken = boost::none) override;

    Future<std::shared_ptr<Session>> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext) override;

#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                              bool asyncOCSPStaple) override {
        MONGO_UNIMPLEMENTED;
    };
#endif

    const std::vector<HostAndPort>& getListeningAddresses() const override;

    SessionManager* getSessionManager() const override {
        return nullptr;
    }

    std::shared_ptr<SessionManager> getSharedSessionManager() const override {
        return {};
    }

    ReactorHandle getReactor(WhichReactor which) override {
        switch (which) {
            case WhichReactor::kNewReactor:
                return std::make_shared<GRPCReactor>();
            case WhichReactor::kEgress:
                return _reactor;
            case WhichReactor::kIngress:
                MONGO_UNIMPLEMENTED;
        }
        MONGO_UNREACHABLE;
    }

    bool isIngress() const override {
        return _options.enableIngress;
    }

    bool isEgress() const override {
        return _options.enableEgress;
    }

private:
    enum class StartupState { kNotStarted, kSetup, kStarted, kShutDown };

    Atomic<StartupState> _startupState;

    std::vector<HostAndPort> _listenAddresses;
    std::shared_ptr<Client> _client;
    ServiceContext* const _svcCtx;
    Options _options;

    std::shared_ptr<GRPCReactor> _reactor;
    stdx::thread _ioThread;

    MockClient::MockResolver _resolver;
    const HostAndPort _mockClientAddress;
};

/**
 * A ServiceContextTest-derived fixture whose TransportLayerManager contains a single, egress-only
 * GRPCTransportLayerMock.
 *
 * Note that tests using this fixture must start a MockServer in order to actually establish streams
 * using this fixture.
 */
class MockGRPCTransportLayerTest : public ServiceContextTest {
public:
    inline static const HostAndPort kServerHostAndPort = HostAndPort("localhost", 12345);

    void setUp() override {
        ServiceContextTest::setUp();

        // Mock resolver that automatically returns the producer end of the test's pipe.
        auto resolver = [&](const HostAndPort&) -> MockRPCQueue::Producer {
            return _pipe.producer;
        };
        auto options = CommandServiceTestFixtures::makeTLOptions();
        options.enableIngress = false;
        options.enableEgress = true;

        auto tl = std::make_unique<GRPCTransportLayerMock>(
            getServiceContext(),
            CommandServiceTestFixtures::makeTLOptions(),
            resolver,
            HostAndPort(MockStubTestFixtures::kClientAddress));

        getServiceContext()->setTransportLayerManager(
            std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));
        uassertStatusOK(getServiceContext()->getTransportLayerManager()->setup());
        uassertStatusOK(getServiceContext()->getTransportLayerManager()->start());
    }

    void tearDown() override {
        getServiceContext()->getTransportLayerManager()->shutdown();
        ServiceContextTest::tearDown();
    }

    void runTestWithMockServer(std::function<void(void)> test) {
        auto server = std::make_unique<MockServer>(std::move(_pipe.consumer));
        unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
            server->start(
                monitor,
                [&](auto session) { session->setTerminationStatus(Status::OK()); },
                std::make_unique<WireVersionProvider>());
            ON_BLOCK_EXIT([&] { server->shutdown(); });
            ASSERT_DOES_NOT_THROW(test());
        });
    }

private:
    MockRPCQueue::Pipe _pipe;
    unittest::MinimumLoggedSeverityGuard networkSeverityGuard{logv2::LogComponent::kNetwork,
                                                              logv2::LogSeverity::Debug(4)};
};

}  // namespace grpc
}  // namespace mongo::transport
