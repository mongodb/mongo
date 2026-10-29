// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/database_sharding_metadata_accessor.h"

#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

class DatabaseShardingMetadataAccessorTest : public ServiceContextTest {
public:
    static DatabaseName makeTestDbName() {
        return DatabaseName::createDatabaseName_forTest(boost::none, "test");
    }
};

TEST_F(DatabaseShardingMetadataAccessorTest, DefaultStateIsClean) {
    auto opCtx = cc().makeOperationContext();
    auto dbName = makeTestDbName();
    DatabaseShardingMetadataAccessor cache{dbName};

    ASSERT_EQ(cache.getDbName(), dbName);

    // Should succeed under default read access.
    ASSERT_FALSE(cache.getDbPrimaryShard(opCtx.get()));
    ASSERT_FALSE(cache.getDbVersion(opCtx.get()));
    ASSERT_FALSE(cache.isMovePrimaryInProgress());
}

TEST_F(DatabaseShardingMetadataAccessorTest, MetadataRoundTrip) {
    auto opCtx = cc().makeOperationContext();
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};

    cache.setAccessType(opCtx.get(), DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);

    ShardId primaryShard("shardA");
    DatabaseVersion version{UUID::gen(), Timestamp(1, 0)};

    cache.setDbMetadata(opCtx.get(), primaryShard, version);
    cache.setMovePrimaryInProgress(opCtx.get());

    // Return to reader mode and verify the values.
    cache.setAccessType(opCtx.get(), DatabaseShardingMetadataAccessor::AccessType::kReadAccess);

    ASSERT_TRUE(cache.getDbPrimaryShard(opCtx.get()));
    ASSERT_EQ(*cache.getDbPrimaryShard(opCtx.get()), primaryShard);

    ASSERT_TRUE(cache.getDbVersion(opCtx.get()));
    ASSERT_EQ(*cache.getDbVersion(opCtx.get()), version);

    ASSERT_TRUE(cache.isMovePrimaryInProgress());

    // Clear everything again.
    cache.setAccessType(opCtx.get(), DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);
    cache.clearDbMetadata(opCtx.get());
    cache.unsetMovePrimaryInProgress(opCtx.get());

    cache.setAccessType(opCtx.get(), DatabaseShardingMetadataAccessor::AccessType::kReadAccess);
    ASSERT_FALSE(cache.getDbPrimaryShard(opCtx.get()));
    ASSERT_FALSE(cache.getDbVersion(opCtx.get()));
    ASSERT_FALSE(cache.isMovePrimaryInProgress());
}

TEST_F(DatabaseShardingMetadataAccessorTest, UnsafeSetterBypassesAccessControl) {
    auto opCtx = cc().makeOperationContext();
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};

    ShardId primaryShard("shardB");
    DatabaseVersion version{UUID::gen(), Timestamp(2, 0)};
    cache.setDbMetadata_UNSAFE(opCtx.get(), primaryShard, version);

    // Even in default (read) access mode, reading is allowed
    ASSERT_TRUE(cache.getDbPrimaryShard(opCtx.get()));
    ASSERT_EQ(*cache.getDbPrimaryShard(opCtx.get()), primaryShard);

    ASSERT_TRUE(cache.getDbVersion(opCtx.get()));
    ASSERT_EQ(*cache.getDbVersion(opCtx.get()), version);
}

using DatabaseShardingMetadataAccessorTestDeathTest = DatabaseShardingMetadataAccessorTest;
DEATH_TEST_F(DatabaseShardingMetadataAccessorTestDeathTest,
             MutatorsRequireWriteAccess,
             "Tripwire assertion") {
    auto opCtx = cc().makeOperationContext();
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};
    ShardId primaryShard("shardC");
    DatabaseVersion version{UUID::gen(), Timestamp(3, 0)};

    // Should tassert due to default (read) access
    cache.setDbMetadata(opCtx.get(), primaryShard, version);
}

DEATH_TEST_F(DatabaseShardingMetadataAccessorTestDeathTest,
             AccessorsDisallowedDuringWrite,
             "Tripwire assertion") {
    auto opCtx = cc().makeOperationContext();
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};
    cache.setAccessType(opCtx.get(), DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);

    // Should tassert due to write-only access
    cache.getDbVersion(opCtx.get());
}

TEST_F(DatabaseShardingMetadataAccessorTest, BypassWriteAccessCheck) {
    auto opCtx = cc().makeOperationContext();
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};
    ShardId primaryShard("shardC");
    DatabaseVersion version{UUID::gen(), Timestamp(3, 0)};
    BypassDatabaseMetadataAccess bypass{opCtx.get(),
                                        BypassDatabaseMetadataAccess::Type::kWriteOnly};  // NOLINT
    // Should tassert due to default (read) access
    cache.setDbMetadata(opCtx.get(), primaryShard, version);
}

TEST_F(DatabaseShardingMetadataAccessorTest, BypassReadAccessCheck) {
    auto opCtx = cc().makeOperationContext();
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};
    cache.setAccessType(opCtx.get(), DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);
    BypassDatabaseMetadataAccess bypass{opCtx.get(),
                                        BypassDatabaseMetadataAccess::Type::kReadOnly};  // NOLINT
    // Should be allowed with the bypass object.
    cache.getDbVersion(opCtx.get());
}

DEATH_TEST_F(DatabaseShardingMetadataAccessorTestDeathTest,
             BypassWriteAccessDeniedWithReadBypass,
             "Tripwire assertion") {
    auto opCtx = cc().makeOperationContext();
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};
    ShardId primaryShard("shardC");
    DatabaseVersion version{UUID::gen(), Timestamp(3, 0)};
    BypassDatabaseMetadataAccess bypass{opCtx.get(),
                                        BypassDatabaseMetadataAccess::Type::kReadOnly};  // NOLINT
    // Should tassert due to default (read) access
    cache.setDbMetadata(opCtx.get(), primaryShard, version);
}

DEATH_TEST_F(DatabaseShardingMetadataAccessorTestDeathTest,
             BypassReadAccessDeniedWithWriteBypass,
             "Tripwire assertion") {
    auto opCtx = cc().makeOperationContext();
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};
    cache.setAccessType(opCtx.get(), DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);
    BypassDatabaseMetadataAccess bypass{opCtx.get(),
                                        BypassDatabaseMetadataAccess::Type::kWriteOnly};  // NOLINT
    // Should be allowed with the bypass object.
    cache.getDbVersion(opCtx.get());
}

}  // namespace
}  // namespace mongo
