// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace mongo {
namespace {


ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly);

using AddShardToZoneTest = ConfigServerTestFixture;

TEST_F(AddShardToZoneTest, AddSingleZoneToExistingShardShouldSucceed) {
    ShardType shard;
    shard.setHandle(ShardHandle{ShardId("a"), boost::none});
    shard.setHost("a:1234");

    setupShards({shard});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->addShardToZone(operationContext(), shard.getName(), "z"));
    auto shardDocStatus = getShardDoc(operationContext(), shard.getName());
    ASSERT_OK(shardDocStatus.getStatus());

    auto shardDoc = shardDocStatus.getValue();
    auto tags = shardDoc.getTags();
    ASSERT_EQ(1u, tags.size());
    ASSERT_EQ("z", tags.front());
}

TEST_F(AddShardToZoneTest, AddZoneToShardWithSameTagShouldSucceed) {
    ShardType shard;
    shard.setHandle(ShardHandle{ShardId("a"), boost::none});
    shard.setHost("a:1234");
    shard.setTags({"x", "y"});

    setupShards({shard});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->addShardToZone(operationContext(), shard.getName(), "x"));

    auto shardDocStatus = getShardDoc(operationContext(), shard.getName());
    ASSERT_OK(shardDocStatus.getStatus());

    auto shardDoc = shardDocStatus.getValue();
    auto tags = shardDoc.getTags();
    ASSERT_EQ(2u, tags.size());
    ASSERT_EQ("x", tags.front());
    ASSERT_EQ("y", tags.back());
}

TEST_F(AddShardToZoneTest, AddZoneToShardWithNewTagShouldAppend) {
    ShardType shard;
    shard.setHandle(ShardHandle{ShardId("a"), boost::none});
    shard.setHost("a:1234");
    shard.setTags({"x"});

    setupShards({shard});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->addShardToZone(operationContext(), shard.getName(), "y"));

    auto shardDocStatus = getShardDoc(operationContext(), shard.getName());
    ASSERT_OK(shardDocStatus.getStatus());

    auto shardDoc = shardDocStatus.getValue();
    auto tags = shardDoc.getTags();
    ASSERT_EQ(2u, tags.size());
    ASSERT_EQ("x", tags.front());
    ASSERT_EQ("y", tags.back());
}

TEST_F(AddShardToZoneTest, AddSingleZoneToNonExistingShardShouldFail) {
    ShardType shard;
    shard.setHandle(ShardHandle{ShardId("a"), boost::none});
    shard.setHost("a:1234");

    setupShards({shard});

    auto status = ShardingCatalogManager::get(operationContext())
                      ->addShardToZone(operationContext(), "b", "z");
    ASSERT_EQ(ErrorCodes::ShardNotFound, status);
}

}  // unnamed namespace
}  // namespace mongo
