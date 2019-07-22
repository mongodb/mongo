/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session_for_test.h"
#include "mongo/db/auth/authz_manager_external_state_local.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/platform/basic.h"
#include "mongo/transport/service_entry_point_impl.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    static mongo::ServiceContext* serviceContext;
    static mongo::ServiceContext::UniqueClient client;
    static mongo::transport::TransportLayerMock transportLayer;
    static mongo::transport::SessionHandle session;
    static std::unique_ptr<mongo::AuthzManagerExternalStateMock> localExternalState;
    static mongo::AuthzManagerExternalStateMock* externalState;
    static std::unique_ptr<mongo::AuthorizationManagerImpl> localAuthzManager;
    static mongo::AuthorizationManagerImpl* authzManager;

    static std::unique_ptr<mongo::repl::ReplicationCoordinatorMock> replCoord;
    static const mongo::LogicalTime kInMemoryLogicalTime =
        mongo::LogicalTime(mongo::Timestamp(3, 1));

    static const auto ret = [&]() {
        auto ret = mongo::runGlobalInitializers(0, nullptr, nullptr);
        invariant(ret.isOK());

        setGlobalServiceContext(mongo::ServiceContext::make());
        session = transportLayer.createSession();

        serviceContext = mongo::getGlobalServiceContext();
        serviceContext->setServiceEntryPoint(
            std::make_unique<mongo::ServiceEntryPointMongod>(serviceContext));
        client = serviceContext->makeClient("test", session);
        // opCtx = serviceContext->makeOperationContext(client.get());

        localExternalState = std::make_unique<mongo::AuthzManagerExternalStateMock>();
        externalState = localExternalState.get();
        localAuthzManager = std::make_unique<mongo::AuthorizationManagerImpl>(
            std::move(localExternalState),
            mongo::AuthorizationManagerImpl::InstallMockForTestingOrAuthImpl{});

        authzManager = localAuthzManager.get();
        externalState->setAuthorizationManager(authzManager);
        authzManager->setAuthEnabled(true);
        mongo::AuthorizationManager::set(serviceContext, std::move(localAuthzManager));

        replCoord = std::make_unique<mongo::repl::ReplicationCoordinatorMock>(serviceContext);
        ASSERT_OK(replCoord->setFollowerMode(mongo::repl::MemberState::RS_PRIMARY));
        mongo::repl::ReplicationCoordinator::set(mongo::getGlobalServiceContext(),
                                                 std::move(replCoord));

        return ret;
    }();

    if (Size < sizeof(mongo::MSGHEADER::Value)) {
        return 0;
    }
    mongo::ServiceContext::UniqueOperationContext opCtx =
        serviceContext->makeOperationContext(client.get());
    auto logicalClock = std::make_unique<mongo::LogicalClock>(serviceContext);
    logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
    mongo::LogicalClock::set(serviceContext, std::move(logicalClock));

    int new_size = Size + sizeof(int);
    auto sb = mongo::SharedBuffer::allocate(new_size);
    memcpy(sb.get(), &new_size, sizeof(int));
    memcpy(sb.get() + sizeof(int), Data, Size);
    mongo::Message msg(std::move(sb));

    try {
        serviceContext->getServiceEntryPoint()->handleRequest(opCtx.get(), msg);
    } catch (const mongo::AssertionException&) {
        // We need to catch exceptions caused by invalid inputs
    }

    return 0;
}
