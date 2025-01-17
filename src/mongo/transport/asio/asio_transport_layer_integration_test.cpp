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

#include "mongo/base/string_data.h"
#include "mongo/client/connection_string.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/transport_layer_integration_test_fixture.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/log_test.h"
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

TEST_F(AsioTransportLayerTest, HTTPRequestGetsHTTPError) {
    auto connectionString = unittest::getFixtureConnectionString();
    auto server = connectionString.getServers().front();

    asio::io_context ioContext;
    asio::ip::tcp::resolver resolver(ioContext);
    asio::ip::tcp::socket socket(ioContext);

    LOGV2(23028, "Connecting to server", "server"_attr = server);
    auto resolverIt = resolver.resolve(server.host(), std::to_string(server.port()));
    asio::connect(socket, resolverIt);

    LOGV2(23029, "Sending HTTP request");
    std::string httpReq = str::stream() << "GET /\r\n"
                                           "Host: "
                                        << server
                                        << "\r\n"
                                           "User-Agent: MongoDB Integration test\r\n"
                                           "Accept: */*";
    asio::write(socket, asio::buffer(httpReq.data(), httpReq.size()));

    LOGV2(23030, "Waiting for response");
    std::array<char, 256> httpRespBuf;
    std::error_code ec;
    auto size = asio::read(socket, asio::buffer(httpRespBuf.data(), httpRespBuf.size()), ec);
    StringData httpResp(httpRespBuf.data(), size);

    LOGV2(23031, "Received http response", "response"_attr = httpResp);
    ASSERT_TRUE(httpResp.startsWith("HTTP/1.0 200 OK"));

// Why oh why can't ASIO unify their error codes
#ifdef _WIN32
    ASSERT_EQ(ec, asio::error::connection_reset);
#else
    ASSERT_EQ(ec, asio::error::eof);
#endif
}

class AsioAsyncClientIntegrationTest : public AsyncClientIntegrationTestFixture {
public:
    void setUp() override {
        AsyncClientIntegrationTestFixture::setUp();

        auto connectionString = unittest::getFixtureConnectionString();
        auto server = connectionString.getServers().front();

        auto sc = getGlobalServiceContext();
        auto tl = sc->getTransportLayerManager()->getDefaultEgressLayer();
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
