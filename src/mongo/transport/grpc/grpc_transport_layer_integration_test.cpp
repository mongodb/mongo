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


#include <string>
#include <system_error>
#include <vector>

#include "mongo/client/connection_string.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_integration_test_fixture.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/log_test.h"

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
