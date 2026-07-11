// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

class ClientTest : public unittest::Test, public ScopedGlobalServiceContextForTest {
public:
    constexpr static auto kClientName1 = "foo";
    constexpr static auto kClientName2 = "bar";

    auto makeClient(std::string desc = "ClientTest",
                    const std::shared_ptr<transport::Session>& session = nullptr) {
        return getService()->makeClient(std::move(desc), session);
    }
};

TEST_F(ClientTest, UuidsAreDifferent) {
    // This test trivially asserts that the uuid for two Client instances are different. This is not
    // intended to test the efficacy of our uuid generation. Instead, this is to make sure that we
    // are not default constructing or reusing the same UUID for all Client instances.
    auto client1 = makeClient(kClientName1);
    auto client2 = makeClient(kClientName2);

    ASSERT_NE(client1->getUUID(), client2->getUUID());
}

TEST_F(ClientTest, ReportState) {
    std::string desc = "ReportStateTestClient";
    auto client = makeClient(desc);

    BSONObjBuilder bob;
    client->reportState(bob);

    auto stateObj = bob.done();
    ASSERT_EQ(stateObj.getStringField("desc"), desc);
}

TEST_F(ClientTest, InitThread) {
    ASSERT_FALSE(haveClient());
    Client::initThread("ThreadName", getService());
    ASSERT_TRUE(haveClient());
    Client::releaseCurrent();
}

TEST_F(ClientTest, SetAndReleaseCurrent) {
    auto clientUniq = makeClient();
    auto clientPtr = clientUniq.get();

    ASSERT_FALSE(haveClient());
    Client::setCurrent(std::move(clientUniq));
    ASSERT_TRUE(haveClient());
    ASSERT_EQ(Client::getCurrent(), clientPtr);

    clientUniq = Client::releaseCurrent();
    ASSERT_FALSE(haveClient());
    ASSERT_EQ(clientUniq.get(), clientPtr);
}

using ClientTestDeathTest = ClientTest;
DEATH_TEST_REGEX_F(ClientTestDeathTest, OverwriteThreadsClient, "Invariant failure.*Client1") {
    auto client1 = makeClient("Client1");
    auto client2 = makeClient("Client2");

    Client::setCurrent(std::move(client1));
    Client::setCurrent(std::move(client2));
}

TEST_F(ClientTest, ShouldNotBeConnectedToPriorityPort) {
    transport::TransportLayerMock transportLayer;
    transportLayer.createSessionHook = [](transport::TransportLayer* tl) {
        return std::make_shared<transport::MockSession>(tl);
    };
    auto mainPortClient = makeClient("mainPortClient", transportLayer.createSession());
    ASSERT_FALSE(mainPortClient->isPriorityPortClient());
}

TEST_F(ClientTest, ShouldBeConnectedToPriorityPort) {
    transport::TransportLayerMock transportLayer;
    transportLayer.createSessionHook = [](transport::TransportLayer* tl) {
        return std::make_shared<transport::MockPrioritySession>(tl);
    };
    auto priorityPortClient = makeClient("priorityPortClient", transportLayer.createSession());
    ASSERT_TRUE(priorityPortClient->isPriorityPortClient());
}

}  // namespace
}  // namespace mongo
