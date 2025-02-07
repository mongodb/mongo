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

#pragma once

#include <memory>

#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/mock_client.h"
#include "mongo/transport/grpc/reactor.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"

namespace mongo::transport::grpc {

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

    AtomicWord<StartupState> _startupState;

    std::vector<HostAndPort> _listenAddresses;
    std::shared_ptr<Client> _client;
    ServiceContext* const _svcCtx;
    Options _options;

    std::shared_ptr<GRPCReactor> _reactor;
    stdx::thread _ioThread;

    // Invalidated after setup().
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

private:
    MockRPCQueue::Pipe _pipe;
    unittest::MinimumLoggedSeverityGuard networkSeverityGuard{logv2::LogComponent::kNetwork,
                                                              logv2::LogSeverity::Debug(4)};
};

}  // namespace mongo::transport::grpc
