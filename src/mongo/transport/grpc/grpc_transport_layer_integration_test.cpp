// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/client/connection_string.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_integration_test_fixture.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/log_test.h"

#include <string>
#include <system_error>
#include <vector>

namespace mongo::transport {
namespace {

class GRPCAsyncClientIntegrationTest : public AsyncClientIntegrationTestFixture {
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
        auto tl = svc->getTransportLayerManager()->getTransportLayer(TransportProtocol::GRPC);
        invariant(tl);
        return tl;
    }

private:
    unittest::MinimumLoggedSeverityGuard networkSeverityGuard{logv2::LogComponent::kNetwork,
                                                              logv2::LogSeverity::Debug(4)};
};

TEST_F(GRPCAsyncClientIntegrationTest, ExhaustHelloShouldReceiveMultipleReplies) {
    testExhaustHelloShouldReceiveMultipleReplies();
}

TEST_F(GRPCAsyncClientIntegrationTest, ExhaustHelloShouldStopOnFailure) {
    testExhaustHelloShouldStopOnFailure();
}

TEST_F(GRPCAsyncClientIntegrationTest, RunCommandRequest) {
    testRunCommandRequest();
}

TEST_F(GRPCAsyncClientIntegrationTest, RunCommandRequestCancel) {
    testRunCommandRequestCancel();
}

TEST_F(GRPCAsyncClientIntegrationTest, RunCommandRequestCancelEarly) {
    testRunCommandRequestCancelEarly();
}

TEST_F(GRPCAsyncClientIntegrationTest, RunCommandRequestCancelSourceDismissed) {
    testRunCommandRequestCancelSourceDismissed();
}

TEST_F(GRPCAsyncClientIntegrationTest, RunCommandCancelBetweenSendAndRead) {
    testRunCommandCancelBetweenSendAndRead();
}

TEST_F(GRPCAsyncClientIntegrationTest, ExhaustCommand) {
    testExhaustCommand();
}

TEST_F(GRPCAsyncClientIntegrationTest, BeginExhaustCommandRequestCancel) {
    testBeginExhaustCommandRequestCancel();
}

TEST_F(GRPCAsyncClientIntegrationTest, BeginExhaustCommandCancelEarly) {
    testBeginExhaustCommandCancelEarly();
}

TEST_F(GRPCAsyncClientIntegrationTest, AwaitExhaustCommandCancel) {
    testAwaitExhaustCommandCancel();
}

TEST_F(GRPCAsyncClientIntegrationTest, AwaitExhaustCommandCancelEarly) {
    testAwaitExhaustCommandCancelEarly();
}

TEST_F(GRPCAsyncClientIntegrationTest, EgressNetworkMetrics) {
    testEgressNetworkMetrics();
}

}  // namespace
}  // namespace mongo::transport
