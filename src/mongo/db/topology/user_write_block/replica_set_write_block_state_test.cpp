// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo {
namespace {

class ReplicaSetWriteBlockStateTest : public ServiceContextMongoDTest {};

struct ReplicaSetWritesBlockRejectedSnapshot {
    long long inserts;
    long long updates;
    long long deletes;
};

auto readReplicaSetWritesBlockRejected(const ReplicaSetWriteBlockState* state) {
    BSONObjBuilder bob;
    state->appendReplicaSetWriteBlockRejectionMetrics(bob);
    BSONObj metrics = bob.obj();
    const auto sub = metrics.getObjectField("replicaSetWritesBlockRejected");
    return ReplicaSetWritesBlockRejectedSnapshot{sub["inserts"].safeNumberLong(),
                                                 sub["updates"].safeNumberLong(),
                                                 sub["deletes"].safeNumberLong()};
}

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
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(), nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert));
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
              static_cast<int>(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace));

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_THROWS_CODE(state->checkReplicaSetWritesAllowed(
                           opCtx.get(), nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert),
                       AssertionException,
                       ErrorCodes::ReplicaSetWritesBlocked);
}

TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingAllowsInternalDatabaseNamespaces) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest("admin.coll"),
        ReplicaSetWriteBlockRejectedWriteOp::kInsert));
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest("config.coll"),
        ReplicaSetWriteBlockRejectedWriteOp::kInsert));
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest("local.coll"),
        ReplicaSetWriteBlockRejectedWriteOp::kInsert));
}

TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingAllowsSystemDotProfile) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB", "system.profile");
    ASSERT(nss.isSystemDotProfile());
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(), nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert));
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
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(), nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert));
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
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(), nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert));
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
                       ErrorCodes::ReplicaSetWritesBlocked);
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


TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingIncrementsOnlyRejectedInsertAndUpdateMetrics) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetWriteBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    const auto before = readReplicaSetWritesBlockRejected(state);

    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    for (int i = 0; i < 2; ++i) {
        ASSERT_THROWS_CODE(state->checkReplicaSetWritesAllowed(
                               opCtx.get(), nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert),
                           AssertionException,
                           ErrorCodes::ReplicaSetWritesBlocked);
    }
    ASSERT_THROWS_CODE(state->checkReplicaSetWritesAllowed(
                           opCtx.get(), nss, ReplicaSetWriteBlockRejectedWriteOp::kUpdate),
                       AssertionException,
                       ErrorCodes::ReplicaSetWritesBlocked);

    const auto after = readReplicaSetWritesBlockRejected(state);
    ASSERT_EQ(after.inserts, before.inserts + 2);
    ASSERT_EQ(after.updates, before.updates + 1);
    ASSERT_EQ(after.deletes, before.deletes);
}

TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingRejectedWritesMetricUnchangedForExemptions) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    const auto before = readReplicaSetWritesBlockRejected(state);

    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest("admin.coll"),
        ReplicaSetWriteBlockRejectedWriteOp::kInsert));
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest("userDB", "system.profile"),
        ReplicaSetWriteBlockRejectedWriteOp::kInsert));

    const auto after = readReplicaSetWritesBlockRejected(state);
    ASSERT_EQ(after.inserts, before.inserts);
    ASSERT_EQ(after.updates, before.updates);
    ASSERT_EQ(after.deletes, before.deletes);
}

TEST_F(ReplicaSetWriteBlockStateTest, WriteBlockingRejectedWritesMetricUnchangedWhenBypassEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});

    const auto before = readReplicaSetWritesBlockRejected(state);

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_DOES_NOT_THROW(state->checkReplicaSetWritesAllowed(
        opCtx.get(), nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert));

    const auto after = readReplicaSetWritesBlockRejected(state);
    ASSERT_EQ(after.inserts, before.inserts);
    ASSERT_EQ(after.updates, before.updates);
    ASSERT_EQ(after.deletes, before.deletes);
}

TEST_F(ReplicaSetWriteBlockStateTest,
       DeletionBlockingIncrementsOnlyRejectedDeletesMetricOnEachBlock) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetDeletionsBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    const auto before = readReplicaSetWritesBlockRejected(state);

    state->enableReplicaSetDeletionsBlocking();

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    for (int i = 0; i < 2; ++i) {
        ASSERT_THROWS_CODE(state->checkReplicaSetDeletionsAllowed(opCtx.get(), nss),
                           AssertionException,
                           ErrorCodes::ReplicaSetWritesBlocked);
    }

    const auto after = readReplicaSetWritesBlockRejected(state);
    ASSERT_EQ(after.inserts, before.inserts);
    ASSERT_EQ(after.updates, before.updates);
    ASSERT_EQ(after.deletes, before.deletes + 2);
}

TEST_F(ReplicaSetWriteBlockStateTest, BlockReplicaSetWritesCommandCountersIncrementPerEnable) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetWriteBlocking();

    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    state->disableReplicaSetWriteBlocking();
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    BSONObjBuilder bob;
    state->appendReplicaSetWritesBlockCounters(bob);
    BSONObj doc = bob.obj();
    const auto sub = doc.getObjectField("replicaSetWritesBlockCounters");
    ASSERT_EQ(sub["InsufficientDiskSpace"].safeNumberLong(), 2);
}

TEST_F(ReplicaSetWriteBlockStateTest, CompactAllowedWhenDeletionsBlockingDisabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetDeletionsBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    ASSERT_OK(state->checkIfCompactAllowedToStart(opCtx.get()));
}

TEST_F(ReplicaSetWriteBlockStateTest, CompactAllowedWhenOnlyWritesBlockedButDeletionsAllowed) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    // blockReplicaSetWrites with allowDeletions = true blocks writes but not deletions. Compact is
    // gated by the allowDeletions flag, so it remains allowed.
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    state->disableReplicaSetDeletionsBlocking();
    ASSERT_OK(state->checkIfCompactAllowedToStart(opCtx.get()));
}

TEST_F(ReplicaSetWriteBlockStateTest, CompactBlockedWhenDeletionsBlockingEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    // blockReplicaSetWrites with allowDeletions = false blocks both writes and deletions, so
    // compact is blocked.
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    state->enableReplicaSetDeletionsBlocking();
    ASSERT_EQ(state->checkIfCompactAllowedToStart(opCtx.get()),
              ErrorCodes::ReplicaSetWritesBlocked);

    state->disableReplicaSetDeletionsBlocking();
    ASSERT_OK(state->checkIfCompactAllowedToStart(opCtx.get()));
}

TEST_F(ReplicaSetWriteBlockStateTest, CompactAllowedWhenBypassEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    state->enableReplicaSetDeletionsBlocking();

    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    ASSERT_OK(state->checkIfCompactAllowedToStart(opCtx.get()));
}

TEST_F(ReplicaSetWriteBlockStateTest, ConvertToCappedAllowedWhenWriteBlockingDisabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetWriteBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_OK(state->checkIfConvertToCappedAllowedToStart(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, ConvertToCappedBlockedWheneverWriteBlockingEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");

    // convertToCapped is blocked whenever the replica set write block is enabled,
    // regardless of the allowDeletions flag.
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    // allowDeletions = false (deletions blocked) -> blocked.
    state->enableReplicaSetDeletionsBlocking();
    ASSERT_EQ(state->checkIfConvertToCappedAllowedToStart(opCtx.get(), nss),
              ErrorCodes::ReplicaSetWritesBlocked);

    // allowDeletions = true (deletions allowed) -> still blocked.
    state->disableReplicaSetDeletionsBlocking();
    ASSERT_EQ(state->checkIfConvertToCappedAllowedToStart(opCtx.get(), nss),
              ErrorCodes::ReplicaSetWritesBlocked);

    // Disabling the write block allows convertToCapped again.
    state->disableReplicaSetWriteBlocking();
    ASSERT_OK(state->checkIfConvertToCappedAllowedToStart(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, ConvertToCappedAllowedOnInternalDb) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    const auto nss = NamespaceString::createNamespaceString_forTest("admin.coll");
    ASSERT_OK(state->checkIfConvertToCappedAllowedToStart(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, ConvertToCappedAllowedWhenBypassEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_OK(state->checkIfConvertToCappedAllowedToStart(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, IncomingMigrationAllowedWhenWriteBlockingDisabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetWriteBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    ASSERT_DOES_NOT_THROW(state->checkIfIncomingMigrationAllowedToStart(opCtx.get()));
}

TEST_F(ReplicaSetWriteBlockStateTest, IncomingMigrationBlockedWhenWriteBlockingEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ASSERT_THROWS_CODE(state->checkIfIncomingMigrationAllowedToStart(opCtx.get()),
                       AssertionException,
                       ErrorCodes::ReplicaSetWritesBlocked);

    state->disableReplicaSetWriteBlocking();
    ASSERT_DOES_NOT_THROW(state->checkIfIncomingMigrationAllowedToStart(opCtx.get()));
}

TEST_F(ReplicaSetWriteBlockStateTest, IncomingMigrationAllowedWhenBypassEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    ASSERT_DOES_NOT_THROW(state->checkIfIncomingMigrationAllowedToStart(opCtx.get()));
}

TEST_F(ReplicaSetWriteBlockStateTest, IndexBuildAllowedWhenIndexBuildBlockingDisabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_OK(state->checkIfIndexBuildAllowedToStart(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, IndexBuildBlockedWhenIndexBuildBlockingEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    state->enableUserIndexBuildBlocking();

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_EQ(state->checkIfIndexBuildAllowedToStart(opCtx.get(), nss),
              ErrorCodes::ReplicaSetWritesBlocked);
}

TEST_F(ReplicaSetWriteBlockStateTest, DisableIndexBuildBlockingRestoresAllowedState) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    state->enableUserIndexBuildBlocking();
    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_EQ(state->checkIfIndexBuildAllowedToStart(opCtx.get(), nss),
              ErrorCodes::ReplicaSetWritesBlocked);

    state->disableUserIndexBuildBlocking();
    ASSERT_OK(state->checkIfIndexBuildAllowedToStart(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, IndexBuildAllowedOnInternalDb) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    state->enableUserIndexBuildBlocking();

    ASSERT_OK(state->checkIfIndexBuildAllowedToStart(
        opCtx.get(), NamespaceString::createNamespaceString_forTest("admin.coll")));
    ASSERT_OK(state->checkIfIndexBuildAllowedToStart(
        opCtx.get(), NamespaceString::createNamespaceString_forTest("config.coll")));
    ASSERT_OK(state->checkIfIndexBuildAllowedToStart(
        opCtx.get(), NamespaceString::createNamespaceString_forTest("local.coll")));
}

TEST_F(ReplicaSetWriteBlockStateTest, IndexBuildAllowedWhenBypassEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableUserIndexBuildBlocking();

    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    const auto nss = NamespaceString::createNamespaceString_forTest("userDB.coll");
    ASSERT_OK(state->checkIfIndexBuildAllowedToStart(opCtx.get(), nss));
}

TEST_F(ReplicaSetWriteBlockStateTest, IncomingReshardingAllowedWhenWriteBlockingDisabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->disableReplicaSetWriteBlocking();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    ASSERT_OK(state->checkIfIncomingReshardingAllowedToStart(opCtx.get()));
}

TEST_F(ReplicaSetWriteBlockStateTest, IncomingReshardingBlockedWhenWriteBlockingEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    ReplicaSetWriteBlockBypass::get(opCtx.get()).set(false);

    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);
    ASSERT_EQ(state->checkIfIncomingReshardingAllowedToStart(opCtx.get()),
              ErrorCodes::ReplicaSetWritesBlocked);

    state->disableReplicaSetWriteBlocking();
    ASSERT_OK(state->checkIfIncomingReshardingAllowedToStart(opCtx.get()));
}

TEST_F(ReplicaSetWriteBlockStateTest, IncomingReshardingAllowedWhenBypassEnabled) {
    auto opCtx = cc().makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);

    auto* state = ReplicaSetWriteBlockState::get(opCtx.get());
    state->enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace);

    auto authSession = AuthorizationSession::get(opCtx->getClient());
    authSession->grantInternalAuthorization();
    ReplicaSetWriteBlockBypass::get(opCtx.get()).setFromMetadata(opCtx.get(), {});
    ASSERT(ReplicaSetWriteBlockBypass::get(opCtx.get()).isEnabled());

    ASSERT_OK(state->checkIfIncomingReshardingAllowedToStart(opCtx.get()));
}

}  // namespace
}  // namespace mongo
