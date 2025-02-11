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

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"

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
        return getServiceContext()
            ->getService(ClusterRole::ShardServer)
            ->makeClient("SessionManagerTest", session);
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

TEST_F(SessionManagerTest, TestActiveClientOperationsOnDelist) {
    auto client = makeClient(transportLayer->createSession());
    ASSERT_EQ(getActiveOperations(), 0);
    {
        auto opCtx = client->makeOperationContext();
        ASSERT_EQ(getActiveOperations(), 1);
        getServiceContext()->killAndDelistOperation(opCtx.get());
        ASSERT_EQ(getActiveOperations(), 0);
    }
    ASSERT_EQ(getActiveOperations(), 0);
}

}  // namespace
}  // namespace mongo::transport
