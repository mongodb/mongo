/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <array>
#include <string>
#include <system_error>
#include <vector>

#include "mongo/base/string_data.h"
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
