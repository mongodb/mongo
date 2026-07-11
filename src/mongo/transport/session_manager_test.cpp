// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/session_manager.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {
namespace {

class SessionManagerTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        auto* svcCtx = getServiceContext();
        setupTransportLayerManager(svcCtx);
    }

    void setupTransportLayerManager(ServiceContext* svcCtx) {
        auto tl = std::make_unique<TransportLayerMock>();
        transportLayer = tl.get();

        auto tlm = std::make_unique<TransportLayerManagerImpl>(std::move(tl));
        auto tlmPtr = tlm.get();
        svcCtx->setTransportLayerManager(std::move(tlm));
        ASSERT_OK(tlmPtr->setup());
        ASSERT_OK(tlmPtr->start());
    }

    auto makeClient(std::shared_ptr<transport::Session> session = nullptr) {
        return getServiceContext()->getService()->makeClient("SessionManagerTest", session);
    }

    std::size_t getActiveOperations() const {
        return transportLayer->getSessionManager()->getActiveOperations();
    }

    TransportLayerMock* transportLayer{nullptr};
};

TEST_F(SessionManagerTest, TestActiveClientOperationsForClientsWithoutSession) {
    auto client = makeClient();
    ASSERT_EQ(getActiveOperations(), 0);
    {
        auto opCtx = client->makeOperationContext();
        ASSERT_EQ(getActiveOperations(), 0);
    }
    ASSERT_EQ(getActiveOperations(), 0);
}

TEST_F(SessionManagerTest, TestActiveClientOperationsForClientsWithSessions) {
    auto client = makeClient(transportLayer->createSession());
    ASSERT_EQ(getActiveOperations(), 0);
    {
        auto opCtx = client->makeOperationContext();
        ASSERT_EQ(getActiveOperations(), 1);
    }
    ASSERT_EQ(getActiveOperations(), 0);
}

TEST_F(SessionManagerTest, TestActiveClientOperationsOnPendingDestruction) {
    auto client = makeClient(transportLayer->createSession());
    ASSERT_EQ(getActiveOperations(), 0);
    {
        auto opCtx = client->makeOperationContext();
        ASSERT_EQ(getActiveOperations(), 1);
        getServiceContext()->markOperationAsPendingDestruction(opCtx.get());
        ASSERT_EQ(getActiveOperations(), 0);
    }
    ASSERT_EQ(getActiveOperations(), 0);
}

}  // namespace
}  // namespace mongo::transport
