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

#include <string>
#include <vector>

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/grpc/mock_client.h"
#include "mongo/transport/grpc/mock_wire_version_provider.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/test_fixtures.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo::transport::grpc {

class GRPCClientTest : public ServiceContextTest {
public:
    virtual void setUp() override {
        getServiceContext()->setPeriodicRunner(makePeriodicRunner(getServiceContext()));
    }

    std::shared_ptr<GRPCClient> makeClient(
        GRPCClient::Options options = CommandServiceTestFixtures::makeClientOptions()) {
        return std::make_shared<GRPCClient>(
            nullptr /* transport layer */, makeClientMetadataDocument(), std::move(options));
    }
};

TEST_F(GRPCClientTest, GRPCClientConnect) {
    std::vector<HostAndPort> addresses = {HostAndPort("localhost", test::kLetKernelChoosePort),
                                          HostAndPort("localhost", test::kLetKernelChoosePort),
                                          HostAndPort(makeUnixSockPath(1234))};

    std::vector<Server::Options> serverOptions;
    for (auto& addr : addresses) {
        auto options = CommandServiceTestFixtures::makeServerOptions();
        options.addresses = {addr};
        serverOptions.push_back(std::move(options));
    }

    auto serverHandler = [&](const auto& options, std::shared_ptr<IngressSession> session) {
        auto swClientMsg = session->sourceMessage();
        auto parsedClientMsg = OpMsg::parse(uassertStatusOK(swClientMsg));
        HostAndPort targetedServer{parsedClientMsg.body.getStringField("remote")};
        const auto& addrs = options.addresses;
        ASSERT_NE(std::find(addrs.begin(), addrs.end(), targetedServer), addrs.end());
        ASSERT_OK(session->sinkMessage(swClientMsg.getValue()));
    };

    auto clientThreadBody = [&](auto& servers, auto& monitor) {
        auto client = makeClient();
        client->start(getServiceContext());

        for (auto& server : servers) {
            for (auto& addr : server->getListeningAddresses()) {
                auto session =
                    client->connect(addr, CommandServiceTestFixtures::kDefaultConnectTimeout, {});
                ASSERT_TRUE(session->isConnected());

                OpMsg msg;
                msg.body = BSON("remote" << addr.toString());
                auto serialized = msg.serialize();
                ASSERT_OK(session->sinkMessage(serialized));

                auto serverResponse = session->sourceMessage();
                ASSERT_OK(serverResponse) << "could not read response from " << addr.toString()
                                          << ": " << session->finish().toString();
                ASSERT_EQ_MSG(serverResponse.getValue(), serialized);
                ASSERT_OK(session->finish());
            }
        }
    };

    CommandServiceTestFixtures::runWithServers(serverOptions, serverHandler, clientThreadBody);
}

TEST_F(GRPCClientTest, GRPCClientConnectNoClientCertificate) {
    auto options = CommandServiceTestFixtures::makeServerOptions();
    options.tlsAllowConnectionsWithoutCertificates = true;

    auto clientThreadBody = [&](auto& server, auto& monitor) {
        GRPCClient::Options options;
        options.tlsCAFile = CommandServiceTestFixtures::kCAFile;
        auto client = makeClient(std::move(options));
        client->start(getServiceContext());

        auto session = client->connect(server.getListeningAddresses().at(0),
                                       CommandServiceTestFixtures::kDefaultConnectTimeout,
                                       {});
        auto msg = makeUniqueMessage();
        ASSERT_OK(session->sinkMessage(msg));
        auto swMsgReceived = session->sourceMessage();
        ASSERT_OK(swMsgReceived);
        ASSERT_EQ_MSG(swMsgReceived.getValue(), msg);
        ASSERT_OK(session->finish());
    };

    CommandServiceTestFixtures::runWithServer(
        CommandServiceTestFixtures::makeEchoHandler(), clientThreadBody, std::move(options));
}

TEST_F(GRPCClientTest, GRPCClientConnectAuthToken) {
    const std::string kAuthToken = "my-auth-token";

    auto serverHandler = [&](std::shared_ptr<IngressSession> session) {
        ASSERT_EQ(session->authToken(), kAuthToken);
    };

    auto clientThreadBody = [&](auto& server, auto&) {
        auto client = makeClient();
        client->start(getServiceContext());
        Client::ConnectOptions options;
        options.authToken = kAuthToken;
        auto session = client->connect(server.getListeningAddresses().at(0),
                                       CommandServiceTestFixtures::kDefaultConnectTimeout,
                                       options);
        ASSERT_OK(session->finish());
    };

    CommandServiceTestFixtures::runWithServer(serverHandler, clientThreadBody);
}

TEST_F(GRPCClientTest, GRPCClientConnectNoAuthToken) {
    auto serverHandler = [&](std::shared_ptr<IngressSession> session) {
        ASSERT_FALSE(session->authToken());
    };

    auto clientThreadBody = [&](auto& server, auto&) {
        auto client = makeClient();
        client->start(getServiceContext());
        auto session = client->connect(server.getListeningAddresses().at(0),
                                       CommandServiceTestFixtures::kDefaultConnectTimeout,
                                       {});
        ASSERT_OK(session->finish());
    };

    CommandServiceTestFixtures::runWithServer(serverHandler, clientThreadBody);
}

TEST_F(GRPCClientTest, GRPCClientMetadata) {
    boost::optional<UUID> clientId;

    auto serverHandler = [&](std::shared_ptr<IngressSession> session) {
        ASSERT_TRUE(session->getClientMetadata());
        ASSERT_BSONOBJ_EQ(session->getClientMetadata()->getDocument(),
                          makeClientMetadataDocument());
        ASSERT_TRUE(session->clientId());
        ASSERT_EQ(session->clientId(), clientId);
    };

    auto clientThreadBody = [&](auto& server, auto&) {
        auto client = makeClient();
        client->start(getServiceContext());
        clientId = client->id();
        auto session = client->connect(server.getListeningAddresses().at(0),
                                       CommandServiceTestFixtures::kDefaultConnectTimeout,
                                       {});
        ASSERT_OK(session->finish());
    };

    CommandServiceTestFixtures::runWithServer(serverHandler, clientThreadBody);
}

TEST_F(GRPCClientTest, GRPCClientShutdown) {
    const int kNumRpcs = 10;
    AtomicWord<int> numRpcsRemaining(kNumRpcs);
    Notification<void> rpcsFinished;

    auto serverHandler = [&](std::shared_ptr<IngressSession> session) {
        const auto status = session->sourceMessage().getStatus().code();
        ASSERT_EQ(status, ErrorCodes::CallbackCanceled);
        ASSERT_TRUE(session->terminationStatus().has_value());
        ASSERT_EQ(*session->terminationStatus(), status);
        ASSERT_FALSE(session->isConnected());

        if (numRpcsRemaining.subtractAndFetch(1) == 0) {
            rpcsFinished.set();
        }
    };

    auto clientThreadBody = [&](auto& server, auto& monitor) {
        mongo::Client::initThread("GRPCClientShutdown", getGlobalServiceContext()->getService());
        auto client = makeClient();
        client->start(getServiceContext());

        std::vector<std::shared_ptr<EgressSession>> sessions;
        for (int i = 0; i < kNumRpcs; i++) {
            sessions.push_back(client->connect(server.getListeningAddresses().at(0),
                                               CommandServiceTestFixtures::kDefaultConnectTimeout,
                                               {}));
        }

        Notification<void> shutdownFinished;
        auto th = monitor.spawn([&] {
            client->shutdown();
            shutdownFinished.set();
        });

        rpcsFinished.get();

        for (auto& session : sessions) {
            ASSERT_TRUE(session->terminationStatus());
            ASSERT_EQ(session->terminationStatus()->code(), ErrorCodes::ShutdownInProgress);
            ASSERT_EQ(session->finish().code(), ErrorCodes::ShutdownInProgress);
        }

        ASSERT_THROWS_CODE(client->connect(server.getListeningAddresses().at(0),
                                           CommandServiceTestFixtures::kDefaultConnectTimeout,
                                           {}),
                           DBException,
                           ErrorCodes::ShutdownInProgress);

        auto opCtx = makeOperationContext();
        ASSERT_FALSE(!!shutdownFinished)
            << "shutdown should not return until all sessions have been destroyed";
        sessions.clear();
        ASSERT_TRUE(shutdownFinished.waitFor(opCtx.get(), Seconds(2)));

        th.join();
    };

    CommandServiceTestFixtures::runWithServer(serverHandler, clientThreadBody);
}

}  // namespace mongo::transport::grpc
