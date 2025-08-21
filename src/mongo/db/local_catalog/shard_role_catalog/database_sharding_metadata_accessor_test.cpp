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

#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_metadata_accessor.h"

#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

class DatabaseShardingMetadataAccessorTest : public unittest::Test {
public:
    static DatabaseName makeTestDbName() {
        return DatabaseName::createDatabaseName_forTest(boost::none, "test");
    }
};

TEST_F(DatabaseShardingMetadataAccessorTest, DefaultStateIsClean) {
    auto dbName = makeTestDbName();
    DatabaseShardingMetadataAccessor cache{dbName};

    ASSERT_EQ(cache.getDbName(), dbName);

    // Should succeed under default read access.
    ASSERT_FALSE(cache.getDbPrimaryShard());
    ASSERT_FALSE(cache.getDbVersion());
    ASSERT_FALSE(cache.isMovePrimaryInProgress());
}

TEST_F(DatabaseShardingMetadataAccessorTest, MetadataRoundTrip) {
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};

    cache.setAccessType(DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);

    ShardId primaryShard("shardA");
    DatabaseVersion version{UUID::gen(), Timestamp(1, 0)};

    cache.setDbMetadata(primaryShard, version);
    cache.setMovePrimaryInProgress();

    // Return to reader mode and verify the values.
    cache.setAccessType(DatabaseShardingMetadataAccessor::AccessType::kReadAccess);

    ASSERT_TRUE(cache.getDbPrimaryShard());
    ASSERT_EQ(*cache.getDbPrimaryShard(), primaryShard);

    ASSERT_TRUE(cache.getDbVersion());
    ASSERT_EQ(*cache.getDbVersion(), version);

    ASSERT_TRUE(cache.isMovePrimaryInProgress());

    // Clear everything again.
    cache.setAccessType(DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);
    cache.clearDbMetadata();
    cache.unsetMovePrimaryInProgress();

    cache.setAccessType(DatabaseShardingMetadataAccessor::AccessType::kReadAccess);
    ASSERT_FALSE(cache.getDbPrimaryShard());
    ASSERT_FALSE(cache.getDbVersion());
    ASSERT_FALSE(cache.isMovePrimaryInProgress());
}

TEST_F(DatabaseShardingMetadataAccessorTest, UnsafeSetterBypassesAccessControl) {
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};

    ShardId primaryShard("shardB");
    DatabaseVersion version{UUID::gen(), Timestamp(2, 0)};
    cache.setDbMetadata_UNSAFE(primaryShard, version);

    // Even in default (read) access mode, reading is allowed
    ASSERT_TRUE(cache.getDbPrimaryShard());
    ASSERT_EQ(*cache.getDbPrimaryShard(), primaryShard);

    ASSERT_TRUE(cache.getDbVersion());
    ASSERT_EQ(*cache.getDbVersion(), version);
}

DEATH_TEST_F(DatabaseShardingMetadataAccessorTest,
             MutatorsRequireWriteAccess,
             "Tripwire assertion") {
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};
    ShardId primaryShard("shardC");
    DatabaseVersion version{UUID::gen(), Timestamp(3, 0)};

    // Should tassert due to default (read) access
    cache.setDbMetadata(primaryShard, version);
}

DEATH_TEST_F(DatabaseShardingMetadataAccessorTest,
             AccessorsDisallowedDuringWrite,
             "Tripwire assertion") {
    DatabaseShardingMetadataAccessor cache{makeTestDbName()};
    cache.setAccessType(DatabaseShardingMetadataAccessor::AccessType::kWriteAccess);

    // Should tassert due to write-only access
    cache.getDbVersion();
}

}  // namespace
}  // namespace mongo
