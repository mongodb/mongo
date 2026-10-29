// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/hello_metrics.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/transport/session_manager_common_mock.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"

#include <fmt/format.h>

namespace mongo::transport {
namespace {

class HelloMetricsTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        auto* svcCtx = getServiceContext();
        setupTransportLayerManager(svcCtx);
        session = transportLayer->createSession();
    }

    void setupTransportLayerManager(ServiceContext* svcCtx) {
        auto tl = std::make_unique<TransportLayerMock>(
            std::make_unique<MockSessionManagerCommon>(getServiceContext()));
        transportLayer = tl.get();

        auto tlm = std::make_unique<TransportLayerManagerImpl>(std::move(tl));
        auto tlmPtr = tlm.get();
        svcCtx->setTransportLayerManager(std::move(tlm));
        ASSERT_OK(tlmPtr->setup());
        ASSERT_OK(tlmPtr->start());
    }

    auto makeClient(std::shared_ptr<transport::Session> session) {
        return getServiceContext()->getService()->makeClient("HelloMetricsTest", session);
    }

    struct ExhaustMetrics {
        bool inExhaustHello;
        bool inExhaustIsMaster;
        size_t numInExhaustHello;
        size_t numInExhaustMaster;
    };

    static constexpr ExhaustMetrics helloExhaustMetrics{true, false, 1, 0};
    static constexpr ExhaustMetrics isMasterExhaustMetrics{false, true, 0, 1};

    void assertExhaustMetricsEqual(ExhaustMetrics expected) {
        ASSERT_EQ(InExhaustHello::get(session.get())->getInExhaustHello(), expected.inExhaustHello);
        ASSERT_EQ(InExhaustHello::get(session.get())->getInExhaustIsMaster(),
                  expected.inExhaustIsMaster);
        ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustHello(),
                  expected.numInExhaustHello);
        ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustIsMaster(),
                  expected.numInExhaustMaster);
    }

    TransportLayerMock* transportLayer{nullptr};
    std::shared_ptr<Session> session;
};

TEST_F(HelloMetricsTest, TwoExhaustHellosIncrementOnce) {
    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kHello);
    assertExhaustMetricsEqual(helloExhaustMetrics);

    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kHello);
    assertExhaustMetricsEqual(helloExhaustMetrics);
}

TEST_F(HelloMetricsTest, ExhaustMasterDecrementsHello) {
    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kHello);
    assertExhaustMetricsEqual(helloExhaustMetrics);

    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kIsMaster);
    assertExhaustMetricsEqual(isMasterExhaustMetrics);
}

TEST_F(HelloMetricsTest, TwoExhaustMastersIncrementOnce) {
    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kIsMaster);
    assertExhaustMetricsEqual(isMasterExhaustMetrics);

    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kIsMaster);
    assertExhaustMetricsEqual(isMasterExhaustMetrics);
}

TEST_F(HelloMetricsTest, ExhaustHelloDecrementsMaster) {
    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kIsMaster);
    assertExhaustMetricsEqual(isMasterExhaustMetrics);

    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kHello);
    assertExhaustMetricsEqual(helloExhaustMetrics);
}

TEST_F(HelloMetricsTest, SessionManagerDecrementsExhaustHelloMetrics) {
    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kHello);
    ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustHello(), 1);

    {
        auto session2 = transportLayer->createSession();
        InExhaustHello::get(session2.get())->setInExhaust(InExhaustHello::Command::kHello);
        ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustHello(), 2);
        ASSERT_DOES_NOT_THROW(transportLayer->deleteSession(session2->id()));
    }

    ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustHello(), 1);
}

TEST_F(HelloMetricsTest, SessionManagerDecrementsExhaustInMasterMetrics) {
    InExhaustHello::get(session.get())->setInExhaust(InExhaustHello::Command::kIsMaster);
    ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustIsMaster(), 1);

    {
        auto session2 = transportLayer->createSession();
        InExhaustHello::get(session2.get())->setInExhaust(InExhaustHello::Command::kIsMaster);
        ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustIsMaster(), 2);
        ASSERT_DOES_NOT_THROW(transportLayer->deleteSession(session2->id()));
    }

    ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustIsMaster(), 1);
}

/**
 * Verifies that operator+= sums all three fields (exhaustIsMaster, exhaustHello,
 * awaitingTopologyChanges) from another HelloMetrics instance into the receiver.
 */
TEST_F(HelloMetricsTest, AdditionOperatorSumsAllFields) {
    auto& metrics = transportLayer->getSessionManager()->helloMetrics;

    // Drive exhaustHello and exhaustIsMaster via InExhaustHello on separate sessions.
    auto sessionForHello = transportLayer->createSession();
    InExhaustHello::get(sessionForHello.get())->setInExhaust(InExhaustHello::Command::kHello);

    auto sessionForIsMaster = transportLayer->createSession();
    InExhaustHello::get(sessionForIsMaster.get())->setInExhaust(InExhaustHello::Command::kIsMaster);

    metrics.incrementNumAwaitingTopologyChanges();
    metrics.incrementNumAwaitingTopologyChanges();

    HelloMetrics accumulated;
    accumulated += metrics;

    ASSERT_EQ(accumulated.getNumExhaustHello(), 1);
    ASSERT_EQ(accumulated.getNumExhaustIsMaster(), 1);
    ASSERT_EQ(accumulated.getNumAwaitingTopologyChanges(), 2);

    // Fail if a new field is added to HelloMetrics without being covered above.
    BSONObjBuilder bob;
    accumulated.serialize(&bob);
    ASSERT_EQ(bob.obj().nFields(), 3) << "new HelloMetrics field needs a sum assertion above";
}

}  // namespace
}  // namespace mongo::transport
