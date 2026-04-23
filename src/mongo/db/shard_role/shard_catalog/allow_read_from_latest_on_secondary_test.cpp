/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
