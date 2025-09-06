/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/ddl/direct_connection_ddl_hook.h"

#include "mongo/db/auth/authorization_session_for_test.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

#include <set>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.foo");
const NamespaceString kAnotherNss = NamespaceString::createNamespaceString_forTest("test2.bar");

void makeAuthorizedForDirectOps(Client* client) {
    auto localSessionState = std::make_unique<AuthzSessionExternalStateMock>(client);
    auto authzSession =
        std::make_unique<AuthorizationSessionForTest>(std::move(localSessionState), client);
    authzSession->assumePrivilegesForBuiltinRole(RoleName{"root", "admin"});
    AuthorizationSession::set(client, std::move(authzSession));
}

void makeUnauthorizedForDirectOps(Client* client) {
    auto localSessionState = std::make_unique<AuthzSessionExternalStateMock>(client);
    auto authzSession =
        std::make_unique<AuthorizationSessionForTest>(std::move(localSessionState), client);
    authzSession->assumePrivilegesForBuiltinRole(RoleName{"clusterAdmin", "admin"});
    AuthorizationSession::set(client, std::move(authzSession));
}

class DirectConnectionDDLHookTestReplicaSet : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        operationContext = cc().makeOperationContext();

        const auto& client = operationContext.get()->getClient();
        authzManager = AuthorizationManager::get(client->getService());
        AuthorizationManager::get(getServiceContext()->getService())->setAuthEnabled(true);
    }

protected:
    AuthorizationManager* authzManager;
    ServiceContext::UniqueOperationContext operationContext;
    RAIIServerParameterControllerForTest featureFlagController{
        "featureFlagPreventDirectShardDDLsDuringPromotion", true};
};

TEST_F(DirectConnectionDDLHookTestReplicaSet, BasicRegisterUnauthorizedShardingDisabled) {
    makeUnauthorizedForDirectOps(operationContext.get()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext.get(), std::vector<NamespaceString>{kNss});
    stdx::unordered_map<OperationId, int> expectedMap{{operationContext.get()->getOpID(), 1}};
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);
}

class DirectConnectionDDLHookTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        ReplicaSetDDLTracker::create(getServiceContext());

        const auto& client = operationContext()->getClient();
        authzManager = AuthorizationManager::get(client->getService());
        AuthorizationManager::get(getServiceContext()->getService())->setAuthEnabled(true);
    }

    auto makeClient(std::string desc = "DirectConnectionDDLHookTest",
                    std::shared_ptr<transport::Session> session = nullptr) {
        return getServiceContext()->getService()->makeClient(desc, session);
    }

protected:
    AuthorizationManager* authzManager;
    RAIIServerParameterControllerForTest featureFlagController{
        "featureFlagPreventDirectShardDDLsDuringPromotion", true};
};

TEST_F(DirectConnectionDDLHookTest, BasicRegisterOpAuthorizedDirectShardOps) {
    makeAuthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});
    stdx::unordered_map<OperationId, int> expectedMap{{operationContext()->getOpID(), 1}};
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);
}

TEST_F(DirectConnectionDDLHookTest, BasicRegisterOpUnauthorized) {
    makeUnauthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    ASSERT_THROWS_CODE(hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss}),
                       DBException,
                       ErrorCodes::Unauthorized);
}

TEST_F(DirectConnectionDDLHookTest, BasicRegisterOpNoAuth) {
    AuthorizationManager::get(getServiceContext()->getService())->setAuthEnabled(false);

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});
    stdx::unordered_map<OperationId, int> expectedMap{{operationContext()->getOpID(), 1}};
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);
}

TEST_F(DirectConnectionDDLHookTest, RegisterOpSessionsCollection) {
    makeUnauthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(),
                    std::vector<NamespaceString>{NamespaceString::kLogicalSessionsNamespace});
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, RegisterMultiple) {
    makeAuthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});

    auto secondClient = makeClient();
    auto secondOpCtx = secondClient->makeOperationContext();
    makeAuthorizedForDirectOps(secondOpCtx.get()->getClient());

    hook.onBeginDDL(secondOpCtx.get(), std::vector<NamespaceString>{kAnotherNss});

    ASSERT_EQ(hook.getOngoingOperations().size(), 2);
    stdx::unordered_map<OperationId, int> expectedMap{{operationContext()->getOpID(), 1},
                                                      {secondOpCtx.get()->getOpID(), 1}};
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);
}

TEST_F(DirectConnectionDDLHookTest, RegisterReEntrant) {
    makeAuthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});

    ASSERT_EQ(hook.getOngoingOperations().size(), 1);
    stdx::unordered_map<OperationId, int> expectedMap{{operationContext()->getOpID(), 2}};
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);

    hook.onEndDDL(operationContext(), std::vector<NamespaceString>{kNss});
    ASSERT_EQ(hook.getOngoingOperations().size(), 1);
    expectedMap.at(operationContext()->getOpID()) = 1;
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);

    hook.onEndDDL(operationContext(), std::vector<NamespaceString>{kNss});
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, BasicDeRegisterOp) {
    makeAuthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});
    hook.onEndDDL(operationContext(), std::vector<NamespaceString>{kNss});
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, DeRegisterEmpty) {
    DirectConnectionDDLHook hook;
    ASSERT_TRUE(hook.getOngoingOperations().empty());
    hook.onEndDDL(operationContext(), std::vector<NamespaceString>{kNss});
    ASSERT_TRUE(hook.getOngoingOperations().empty());
    hook.onEndDDL(operationContext(),
                  std::vector<NamespaceString>{NamespaceString::kLogicalSessionsNamespace});
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, DeRegisterWrongOpId) {
    makeAuthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});

    auto secondClient = makeClient();
    auto secondOpCtx = secondClient->makeOperationContext();
    makeAuthorizedForDirectOps(secondOpCtx.get()->getClient());

    hook.onEndDDL(secondOpCtx.get(), std::vector<NamespaceString>{kAnotherNss});
    ASSERT_FALSE(hook.getOngoingOperations().empty());
    hook.onEndDDL(operationContext(), std::vector<NamespaceString>{kNss});
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, GetWaitForDrainedFutureNoOngoing) {
    DirectConnectionDDLHook hook;
    ASSERT_TRUE(hook.getWaitForDrainedFuture(operationContext()).isReady());
}

TEST_F(DirectConnectionDDLHookTest, GetWaitForDrainedFutureOneOp) {
    makeAuthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});
    auto future = hook.getWaitForDrainedFuture(operationContext());
    ASSERT_FALSE(future.isReady());
    hook.onEndDDL(operationContext(), std::vector<NamespaceString>{kNss});
    ASSERT_TRUE(future.isReady());
    ASSERT_TRUE(hook.getWaitForDrainedFuture(operationContext()).isReady());
}

TEST_F(DirectConnectionDDLHookTest, GetWaitForDrainedFutureMultipleOps) {
    makeAuthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});

    auto secondClient = makeClient();
    auto secondOpCtx = secondClient->makeOperationContext();
    makeAuthorizedForDirectOps(secondOpCtx.get()->getClient());

    hook.onBeginDDL(secondOpCtx.get(), std::vector<NamespaceString>{kAnotherNss});

    auto future = hook.getWaitForDrainedFuture(operationContext());
    ASSERT_FALSE(future.isReady());
    hook.onEndDDL(secondOpCtx.get(), std::vector<NamespaceString>{kAnotherNss});
    ASSERT_FALSE(future.isReady());
    hook.onEndDDL(operationContext(), std::vector<NamespaceString>{kNss});
    ASSERT_TRUE(future.isReady());
}

TEST_F(DirectConnectionDDLHookTest, GetWaitForDrainedFutureReEntrant) {
    makeAuthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});

    auto future = hook.getWaitForDrainedFuture(operationContext());
    ASSERT_FALSE(future.isReady());

    hook.onEndDDL(operationContext(), std::vector<NamespaceString>{kNss});
    ASSERT_FALSE(future.isReady());

    hook.onEndDDL(operationContext(), std::vector<NamespaceString>{kNss});
    ASSERT_TRUE(future.isReady());
}

TEST_F(DirectConnectionDDLHookTest, WaitForDrainedAllowedOpUnregistered) {
    makeAuthorizedForDirectOps(operationContext()->getClient());

    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), std::vector<NamespaceString>{kNss});

    auto future = hook.getWaitForDrainedFuture(operationContext());

    auto secondClient = makeClient();
    auto secondOpCtx = secondClient->makeOperationContext();
    makeAuthorizedForDirectOps(secondOpCtx.get()->getClient());

    hook.onBeginDDL(secondOpCtx.get(), std::vector<NamespaceString>{kAnotherNss});
    stdx::unordered_map<OperationId, int> expectedMap{{operationContext()->getOpID(), 1}};
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);
}

}  // namespace
}  // namespace mongo
