/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class ReplicaSetWriteBlockStateTest : public ServiceContextMongoDTest {};

TEST_F(ReplicaSetWriteBlockStateTest, GetFromServiceContextMatchesGetFromOperationContext) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    auto* fromCtx = ReplicaSetWriteBlockState::get(opCtx.get());
    auto* fromService = ReplicaSetWriteBlockState::get(getServiceContext());
    ASSERT_EQ(fromCtx, fromService);
}

TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingDisabledAllowsUserNamespace) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetWriteBlocking();
    ASSERT_FALSE(state->isReplicaSetWriteBlockingEnabled());

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingEnabledBlocksUserNamespace) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetWriteBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ASSERT_TRUE(state->isReplicaSetWriteBlockingEnabled());
    ASSERT_EQ(state->getReplicaSetWriteBlockingReason(opCtx.get()),
              ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_THROWS_CODE(state->checkReplicaSetWritesAllowed(opCtx.get(), nss),
                       AssertionException,
                       ErrorCodes::UserWritesBlocked);
}

TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingAllowsInternalDatabaseNamespaces) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(), NamespaceString::createNamespaceString_forTest("admin.coll")));
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(), NamespaceString::createNamespaceString_forTest("config.coll")));
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(), NamespaceString::createNamespaceString_forTest("local.coll")));
}

TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingAllowsSystemDotProfile) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB", "system.profile");
    ASSERT(nss.isSystemDotProfile());
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingAllowsWhenBypassEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, DisableWriteBlockingClearsIsEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ASSERT_TRUE(state->isReplicaSetWriteBlockingEnabled());

    state->disableReplicaSetWriteBlocking();
    ASSERT_FALSE(state->isReplicaSetWriteBlockingEnabled());

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, DisableDeletionsBlockingClearsIsEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetDeletionsBlocking();
    ASSERT_TRUE(state->isReplicaSetDeletionsBlockingEnabled_forTest());

    state->disableReplicaSetDeletionsBlocking();
    ASSERT_FALSE(state->isReplicaSetDeletionsBlockingEnabled_forTest());

    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);
    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetDeletionsAllowed(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, DeletionsBlockingBlocksUserNamespace) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetDeletionsBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    state->enableReplicaSetDeletionsBlocking();

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_THROWS_CODE(state->checkReplicaSetDeletionsAllowed(opCtx.get(), nss),
                       AssertionException,
                       ErrorCodes::UserWritesBlocked);
}

TEST_F(ReplicaSetWriteBlockStateTest, DeletionsBlockingAllowsSystemDotProfile) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetDeletionsBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB", "system.profile");
    ASSERT(nss.isSystemDotProfile());
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetDeletionsAllowed(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, DeletionsBlockingAllowsInternalDatabase) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetDeletionsBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    ASSERT_DOES_NOT_THROW(state->checkReplicaSetDeletionsAllowed(
        opCtx.get(), NamespaceString::createNamespaceString_forTest("admin.coll")));
}

TEST_F(ReplicaSetWriteBlockStateTest, DeletionsBlockingAllowsWhenBypassEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetDeletionsBlocking();

    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetDeletionsAllowed(opCtx.get(), nss));
}

}  // namespace
}  // namespace mongo
