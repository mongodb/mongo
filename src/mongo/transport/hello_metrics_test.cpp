/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/transport/hello_metrics.h"

#include <fmt/format.h>

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/transport/session_manager_common_mock.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

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
        return getServiceContext()
            ->getService(ClusterRole::ShardServer)
            ->makeClient("HelloMetricsTest", session);
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
    InExhaustHello::get(session.get())->setInExhaust(true, "hello"_sd);
    assertExhaustMetricsEqual(helloExhaustMetrics);

    InExhaustHello::get(session.get())->setInExhaust(true, "hello"_sd);
    assertExhaustMetricsEqual(helloExhaustMetrics);
}

TEST_F(HelloMetricsTest, ExhaustMasterDecrementsHello) {
    InExhaustHello::get(session.get())->setInExhaust(true, "hello"_sd);
    assertExhaustMetricsEqual(helloExhaustMetrics);

    InExhaustHello::get(session.get())->setInExhaust(true, "isMaster"_sd);
    assertExhaustMetricsEqual(isMasterExhaustMetrics);
}

TEST_F(HelloMetricsTest, TwoExhaustMastersIncrementOnce) {
    InExhaustHello::get(session.get())->setInExhaust(true, "isMaster"_sd);
    assertExhaustMetricsEqual(isMasterExhaustMetrics);

    InExhaustHello::get(session.get())->setInExhaust(true, "isMaster"_sd);
    assertExhaustMetricsEqual(isMasterExhaustMetrics);
}

TEST_F(HelloMetricsTest, ExhaustHelloDecrementsMaster) {
    InExhaustHello::get(session.get())->setInExhaust(true, "isMaster"_sd);
    assertExhaustMetricsEqual(isMasterExhaustMetrics);

    InExhaustHello::get(session.get())->setInExhaust(true, "hello"_sd);
    assertExhaustMetricsEqual(helloExhaustMetrics);
}

TEST_F(HelloMetricsTest, SessionManagerDecrementsExhaustHelloMetrics) {
    InExhaustHello::get(session.get())->setInExhaust(true, "hello"_sd);
    ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustHello(), 1);

    {
        auto session2 = transportLayer->createSession();
        InExhaustHello::get(session2.get())->setInExhaust(true, "hello"_sd);
        ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustHello(), 2);
        ASSERT_DOES_NOT_THROW(transportLayer->deleteSession(session2->id()));
    }

    ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustHello(), 1);
}

TEST_F(HelloMetricsTest, SessionManagerDecrementsExhaustInMasterMetrics) {
    InExhaustHello::get(session.get())->setInExhaust(true, "isMaster"_sd);
    ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustIsMaster(), 1);

    {
        auto session2 = transportLayer->createSession();
        InExhaustHello::get(session2.get())->setInExhaust(true, "isMaster"_sd);
        ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustIsMaster(), 2);
        ASSERT_DOES_NOT_THROW(transportLayer->deleteSession(session2->id()));
    }

    ASSERT_EQ(transportLayer->getSessionManager()->helloMetrics.getNumExhaustIsMaster(), 1);
}

}  // namespace
}  // namespace mongo::transport
