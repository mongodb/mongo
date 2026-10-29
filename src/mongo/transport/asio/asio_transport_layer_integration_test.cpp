// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/client/connection_string.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/transport_layer_integration_test_fixture.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <array>
#include <string>
#include <system_error>
#include <vector>

#include <asio.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::transport {
namespace {

class AsioTransportLayerTest : public unittest::Test {
private:
    unittest::MinimumLoggedSeverityGuard networkSeverityGuard{logv2::LogComponent::kNetwork,
                                                              logv2::LogSeverity::Debug(4)};
};

class AsioAsyncClientIntegrationTest : public AsyncClientIntegrationTestFixture {
public:
    void setUp() override {
        AsyncClientIntegrationTestFixture::setUp();

        auto connectionString = unittest::getFixtureConnectionString();
        auto server = connectionString.getServers().front();

        auto sc = getGlobalServiceContext();
        auto tl = getTransportLayer(sc);
        _reactor = tl->getReactor(transport::TransportLayer::kNewReactor);
        _reactorThread = stdx::thread([&] {
            _reactor->run();
            _reactor->drain();
        });
    }

    void tearDown() override {
        _reactor->stop();
        _reactorThread.join();
    }

    TransportLayer* getTransportLayer(ServiceContext* svc) const override {
        auto tl = svc->getTransportLayerManager()->getTransportLayer(TransportProtocol::MongoRPC);
        invariant(tl);
        return tl;
    }

private:
    unittest::MinimumLoggedSeverityGuard networkSeverityGuard{logv2::LogComponent::kNetwork,
                                                              logv2::LogSeverity::Debug(4)};
};

class AsioAsyncClientIntegrationTestWithBaton : public AsioAsyncClientIntegrationTest {
public:
    void setUp() override {
        AsioAsyncClientIntegrationTest::setUp();
        _client = getGlobalServiceContext()->getService()->makeClient("BatonClient");
        _opCtx = _client->makeOperationContext();
        _baton = _opCtx->getBaton();
    }

    BatonHandle baton() override {
        return _baton;
    }

    Interruptible* interruptible() override {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    BatonHandle _baton;
};

using AsioAsyncClientIntegrationTestWithoutBaton = AsioAsyncClientIntegrationTest;

#define TEST_WITH_AND_WITHOUT_BATON_F(suite, name) \
    TEST_F(suite##WithBaton, name) {               \
        test##name();                              \
    }                                              \
    TEST_F(suite##WithoutBaton, name) {            \
        test##name();                              \
    }

TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, AsyncConnectTimeoutCleansUpSocket);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest,
                              ExhaustHelloShouldReceiveMultipleReplies);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, ExhaustHelloShouldStopOnFailure);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, RunCommandRequest);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, RunCommandRequestCancel);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, RunCommandRequestCancelEarly);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest,
                              RunCommandRequestCancelSourceDismissed);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, RunCommandCancelBetweenSendAndRead);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, ExhaustCommand);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, BeginExhaustCommandRequestCancel);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, BeginExhaustCommandCancelEarly);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, AwaitExhaustCommandCancel);
TEST_WITH_AND_WITHOUT_BATON_F(AsioAsyncClientIntegrationTest, AwaitExhaustCommandCancelEarly);

TEST_F(AsioAsyncClientIntegrationTest, EgressNetworkMetrics) {
    testEgressNetworkMetrics();
}

}  // namespace
}  // namespace mongo::transport
