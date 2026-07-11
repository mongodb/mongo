// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/allow_read_from_latest_on_secondary.h"

#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/snapshot_helper.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class AllowReadFromLatestOnSecondaryTest : public CatalogTestFixture {};

TEST_F(AllowReadFromLatestOnSecondaryTest, DisallowedByDefault) {
    ASSERT_FALSE(allowReadFromLatestOnSecondary(operationContext()));
}

TEST_F(AllowReadFromLatestOnSecondaryTest, ResetToFalse) {
    {
        AllowReadFromLatestOnSecondaryBlock_UNSAFE block(operationContext());
        ASSERT_TRUE(allowReadFromLatestOnSecondary(operationContext()));
    }
    ASSERT_FALSE(allowReadFromLatestOnSecondary(operationContext()));
}

TEST_F(AllowReadFromLatestOnSecondaryTest, ResetToTrue) {
    AllowReadFromLatestOnSecondaryBlock_UNSAFE outerBlock(operationContext());
    {
        AllowReadFromLatestOnSecondaryBlock_UNSAFE innerBlock(operationContext());
        ASSERT_TRUE(allowReadFromLatestOnSecondary(operationContext()));
    }
    ASSERT_TRUE(allowReadFromLatestOnSecondary(operationContext()));
}


TEST_F(AllowReadFromLatestOnSecondaryTest, ScopedBlockDeterminesReadSourceForSecondaries) {
    ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    const NamespaceString replicatedNss =
        NamespaceString::createNamespaceString_forTest("test", "coll");

    const auto readSourceInfoBeforeBlock =
        SnapshotHelper::getReadSourceForSecondaryReadsIfNeeded(operationContext(), replicatedNss);
    ASSERT_TRUE(readSourceInfoBeforeBlock.has_value());
    ASSERT_EQ(RecoveryUnit::ReadSource::kLastApplied, readSourceInfoBeforeBlock->readSource);

    {
        AllowReadFromLatestOnSecondaryBlock_UNSAFE block(operationContext());
        const auto readSourceInfoInBlock = SnapshotHelper::getReadSourceForSecondaryReadsIfNeeded(
            operationContext(), replicatedNss);
        ASSERT_TRUE(readSourceInfoInBlock.has_value());
        ASSERT_EQ(RecoveryUnit::ReadSource::kNoTimestamp, readSourceInfoInBlock->readSource);
    }

    const auto readSourceInfoAfterBlock =
        SnapshotHelper::getReadSourceForSecondaryReadsIfNeeded(operationContext(), replicatedNss);
    ASSERT_TRUE(readSourceInfoAfterBlock.has_value());
    ASSERT_EQ(RecoveryUnit::ReadSource::kLastApplied, readSourceInfoAfterBlock->readSource);
}

}  // namespace
}  // namespace mongo
