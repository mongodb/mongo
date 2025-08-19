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

#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

#include <set>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.foo");
const NamespaceString kAnotherNss = NamespaceString::createNamespaceString_forTest("test2.bar");

class DirectConnectionDDLHookTest : public ShardServerTestFixture {
public:
    auto makeClient(std::string desc = "DirectConnectionDDLHookTest",
                    std::shared_ptr<transport::Session> session = nullptr) {
        return getServiceContext()->getService()->makeClient(desc, session);
    }
};

TEST_F(DirectConnectionDDLHookTest, BasicRegisterOp) {
    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), kNss);
    stdx::unordered_map<OperationId, int> expectedMap{{operationContext()->getOpID(), 1}};
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);
}

TEST_F(DirectConnectionDDLHookTest, RegisterOpSessionsCollection) {
    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), NamespaceString::kLogicalSessionsNamespace);
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, RegisterMultiple) {
    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), kNss);
    auto secondClient = makeClient();
    auto secondOpCtx = secondClient->makeOperationContext();
    hook.onBeginDDL(secondOpCtx.get(), kAnotherNss);

    ASSERT_EQ(hook.getOngoingOperations().size(), 2);
    stdx::unordered_map<OperationId, int> expectedMap{{operationContext()->getOpID(), 1},
                                                      {secondOpCtx.get()->getOpID(), 1}};
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);
}

TEST_F(DirectConnectionDDLHookTest, RegisterReEntrant) {
    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), kNss);
    hook.onBeginDDL(operationContext(), kNss);

    ASSERT_EQ(hook.getOngoingOperations().size(), 1);
    stdx::unordered_map<OperationId, int> expectedMap{{operationContext()->getOpID(), 2}};
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);

    hook.onEndDDL(operationContext(), kNss);
    ASSERT_EQ(hook.getOngoingOperations().size(), 1);
    expectedMap.at(operationContext()->getOpID()) = 1;
    ASSERT_EQ(hook.getOngoingOperations(), expectedMap);

    hook.onEndDDL(operationContext(), kNss);
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, BasicDeRegisterOp) {
    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), kNss);
    hook.onEndDDL(operationContext(), kNss);
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, DeRegisterEmpty) {
    DirectConnectionDDLHook hook;
    ASSERT_TRUE(hook.getOngoingOperations().empty());
    hook.onEndDDL(operationContext(), kNss);
    ASSERT_TRUE(hook.getOngoingOperations().empty());
    hook.onEndDDL(operationContext(), NamespaceString::kLogicalSessionsNamespace);
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, DeRegisterWrongOpId) {
    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), kNss);
    auto secondClient = makeClient();
    auto secondOpCtx = secondClient->makeOperationContext();
    hook.onEndDDL(secondOpCtx.get(), kAnotherNss);
    ASSERT_FALSE(hook.getOngoingOperations().empty());
    hook.onEndDDL(operationContext(), kNss);
    ASSERT_TRUE(hook.getOngoingOperations().empty());
}

TEST_F(DirectConnectionDDLHookTest, GetWaitForDrainedFutureNoOngoing) {
    DirectConnectionDDLHook hook;
    ASSERT_TRUE(hook.getWaitForDrainedFuture(operationContext()).isReady());
}

TEST_F(DirectConnectionDDLHookTest, GetWaitForDrainedFutureOneOp) {
    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), kNss);
    auto future = hook.getWaitForDrainedFuture(operationContext());
    ASSERT_FALSE(future.isReady());
    hook.onEndDDL(operationContext(), kNss);
    ASSERT_TRUE(future.isReady());
    ASSERT_TRUE(hook.getWaitForDrainedFuture(operationContext()).isReady());
}

TEST_F(DirectConnectionDDLHookTest, GetWaitForDrainedFutureMultipleOps) {
    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), kNss);
    auto secondClient = makeClient();
    auto secondOpCtx = secondClient->makeOperationContext();
    hook.onBeginDDL(secondOpCtx.get(), kAnotherNss);

    auto future = hook.getWaitForDrainedFuture(operationContext());
    ASSERT_FALSE(future.isReady());
    hook.onEndDDL(secondOpCtx.get(), kAnotherNss);
    ASSERT_FALSE(future.isReady());
    hook.onEndDDL(operationContext(), kNss);
    ASSERT_TRUE(future.isReady());
}

TEST_F(DirectConnectionDDLHookTest, GetWaitForDrainedFutureReEntrant) {
    DirectConnectionDDLHook hook;
    hook.onBeginDDL(operationContext(), kNss);
    hook.onBeginDDL(operationContext(), kNss);

    auto future = hook.getWaitForDrainedFuture(operationContext());
    ASSERT_FALSE(future.isReady());

    hook.onEndDDL(operationContext(), kNss);
    ASSERT_FALSE(future.isReady());

    hook.onEndDDL(operationContext(), kNss);
    ASSERT_TRUE(future.isReady());
}

}  // namespace
}  // namespace mongo
