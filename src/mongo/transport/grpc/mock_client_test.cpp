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
#include "mongo/transport/grpc/mock_client.h"
#include "mongo/transport/grpc/mock_wire_version_provider.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace mongo::transport::grpc {

class MockClientTest : public ServiceContextTest {
public:
    static HostAndPort defaultServerAddress() {
        return HostAndPort("localhost", 1234);
    }

    virtual void setUp() override {
        _reactor = std::make_shared<GRPCReactor>();
    }

    const std::shared_ptr<GRPCReactor>& getReactor() {
        return _reactor;
    }

private:
    std::shared_ptr<GRPCReactor> _reactor;
};

TEST_F(MockClientTest, MockConnect) {
    std::vector<HostAndPort> addresses = {HostAndPort("localhost", 27017),
                                          HostAndPort("localhost", 27018),
                                          HostAndPort(makeUnixSockPath(1234))};

    auto serverHandler = [&](HostAndPort local, std::shared_ptr<IngressSession> session) {
        ASSERT_EQ(session->remote(), HostAndPort(CommandServiceTestFixtures::kMockedClientAddr));

        auto swClientMsg = session->sourceMessage();
        ASSERT_OK(swClientMsg.getStatus());

        auto parsedClientMsg = OpMsg::parse(swClientMsg.getValue());
        HostAndPort targetedServer = HostAndPort(parsedClientMsg.body.getStringField("remote"));
        ASSERT_EQ(targetedServer, local);

        ASSERT_OK(session->sinkMessage(swClientMsg.getValue()));
    };

    auto clientThreadBody = [&](MockClient& client, auto& monitor) {
        client.start(getServiceContext());

        for (auto& addr : addresses) {
            auto session = client.connect(
                addr, getReactor(), CommandServiceTestFixtures::kDefaultConnectTimeout, {});
            ON_BLOCK_EXIT([&] { session->end(); });
            ASSERT_TRUE(session->isConnected());

            OpMsg msg;
            msg.body = BSON("remote" << addr.toString());
            auto serialized = msg.serialize();
            ASSERT_OK(session->sinkMessage(serialized));

            auto serverResponse = session->sourceMessage();
            ASSERT_OK(serverResponse);
            ASSERT_EQ_MSG(serverResponse.getValue(), serialized);
        }
    };

    CommandServiceTestFixtures::runWithMockServers(addresses, serverHandler, clientThreadBody);
}

TEST_F(MockClientTest, MockAuthToken) {
    const std::string kAuthToken = "my-auth-token";

    auto serverHandler = [&](HostAndPort local, std::shared_ptr<IngressSession> session) {
        ASSERT_EQ(session->authToken(), kAuthToken);
    };

    auto clientThreadBody = [&](MockClient& client, auto& monitor) {
        client.start(getServiceContext());
        Client::ConnectOptions options;
        options.authToken = kAuthToken;
        auto session = client.connect(defaultServerAddress(),
                                      getReactor(),
                                      CommandServiceTestFixtures::kDefaultConnectTimeout,
                                      options);
        ASSERT_OK(session->finish());
    };

    CommandServiceTestFixtures::runWithMockServers(
        {defaultServerAddress()}, serverHandler, clientThreadBody);
}

TEST_F(MockClientTest, MockNoAuthToken) {
    auto serverHandler = [&](HostAndPort local, std::shared_ptr<IngressSession> session) {
        ASSERT_FALSE(session->authToken());
    };

    auto clientThreadBody = [&](MockClient& client, auto& monitor) {
        client.start(getServiceContext());
        auto session = client.connect(defaultServerAddress(),
                                      getReactor(),
                                      CommandServiceTestFixtures::kDefaultConnectTimeout,
                                      {});
        ASSERT_OK(session->finish());
    };

    CommandServiceTestFixtures::runWithMockServers(
        {defaultServerAddress()}, serverHandler, clientThreadBody);
}

TEST_F(MockClientTest, MockClientShutdown) {
    const int kNumRpcs = 10;
    AtomicWord<int> numRpcsRemaining(kNumRpcs);
    Notification<void> rpcsFinished;

    auto serverHandler = [&](HostAndPort local, std::shared_ptr<IngressSession> session) {
        const auto status = session->sourceMessage().getStatus();
        ASSERT_EQ(status, ErrorCodes::CallbackCanceled);
        ASSERT_TRUE(session->terminationStatus().has_value());
        ASSERT_EQ(*session->terminationStatus(), status);
        ASSERT_FALSE(session->isConnected());

        if (numRpcsRemaining.subtractAndFetch(1) == 0) {
            rpcsFinished.set();
        }
    };

    auto clientThreadBody = [&](MockClient& client, auto& monitor) {
        mongo::Client::initThread("MockClientShutdown", getGlobalServiceContext()->getService());
        client.start(getServiceContext());

        std::vector<std::shared_ptr<EgressSession>> sessions;
        for (int i = 0; i < kNumRpcs; i++) {
            sessions.push_back(client.connect(defaultServerAddress(),
                                              getReactor(),
                                              CommandServiceTestFixtures::kDefaultConnectTimeout,
                                              {}));
        }

        Notification<void> shutdownFinished;
        auto shutdownThread = monitor.spawn([&] {
            client.shutdown();
            shutdownFinished.set();
        });

        rpcsFinished.get();

        for (auto& session : sessions) {
            ASSERT_TRUE(session->terminationStatus());
            ASSERT_EQ(session->terminationStatus()->code(), ErrorCodes::ShutdownInProgress);
            ASSERT_EQ(session->finish().code(), ErrorCodes::ShutdownInProgress);
        }

        auto opCtx = makeOperationContext();
        ASSERT_TRUE(!shutdownFinished)
            << "shutdown should not return until all sessions have been destroyed";
        sessions.clear();
        ASSERT_TRUE(shutdownFinished.waitFor(opCtx.get(), Seconds(2)));

        shutdownThread.join();
    };

    CommandServiceTestFixtures::runWithMockServers(
        {defaultServerAddress()}, serverHandler, clientThreadBody);
}

TEST_F(MockClientTest, MockClientMetadata) {
    const BSONObj metadataDoc = makeClientMetadataDocument();
    Notification<UUID> clientId;

    auto serverHandler = [&](HostAndPort local, std::shared_ptr<IngressSession> session) {
        ASSERT_TRUE(session->getClientMetadata());
        ASSERT_BSONOBJ_EQ(session->getClientMetadata()->getDocument(), metadataDoc);
        ASSERT_TRUE(session->getRemoteClientId());
        ASSERT_EQ(session->getRemoteClientId(), clientId.get());
    };

    auto clientThreadBody = [&](MockClient& client, auto& monitor) {
        client.start(getServiceContext());
        clientId.set(client.id());
        auto session = client.connect(defaultServerAddress(),
                                      getReactor(),
                                      CommandServiceTestFixtures::kDefaultConnectTimeout,
                                      {});
        ASSERT_OK(session->finish());
    };

    CommandServiceTestFixtures::runWithMockServers(
        {defaultServerAddress()}, serverHandler, clientThreadBody, metadataDoc);
}

TEST_F(MockClientTest, WireVersionGossipping) {
    auto wvProvider = std::make_shared<MockWireVersionProvider>();
    const auto kServerMaxWireVersion = 24;
    wvProvider->setClusterMaxWireVersion(kServerMaxWireVersion);

    auto serverHandler = [&](HostAndPort local, std::shared_ptr<IngressSession> session) {
        while (session->sourceMessage().isOK()) {
            if (!session->sinkMessage(makeUniqueMessage()).isOK()) {
                return;
            }
        }
    };

    auto clientThreadBody = [&](MockClient& client, auto& monitor) {
        client.start(getServiceContext());
        ASSERT_EQ(client.getClusterMaxWireVersion(), util::constants::kMinimumWireVersion);

        auto runTest = [&](int initialWireVersion, int updatedWireVersion) {
            auto session = client.connect(defaultServerAddress(),
                                          getReactor(),
                                          CommandServiceTestFixtures::kDefaultConnectTimeout,
                                          {});
            ASSERT_EQ(client.getClusterMaxWireVersion(), initialWireVersion);
            ASSERT_OK(session->sinkMessage(makeUniqueMessage()));
            ASSERT_EQ(client.getClusterMaxWireVersion(), initialWireVersion);
            ASSERT_OK(session->sourceMessage());
            ASSERT_EQ(client.getClusterMaxWireVersion(), updatedWireVersion);
            ASSERT_OK(session->finish());
        };

        runTest(util::constants::kMinimumWireVersion, kServerMaxWireVersion);
        wvProvider->setClusterMaxWireVersion(kServerMaxWireVersion + 1);
        runTest(kServerMaxWireVersion, kServerMaxWireVersion + 1);
    };

    CommandServiceTestFixtures::runWithMockServers({defaultServerAddress()},
                                                   serverHandler,
                                                   clientThreadBody,
                                                   makeClientMetadataDocument(),
                                                   wvProvider);
}

}  // namespace mongo::transport::grpc
