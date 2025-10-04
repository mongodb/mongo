/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/create_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using unittest::assertGet;

class ShardCollectionTestBase : public ConfigServerTestFixture {
protected:
    const ShardId testPrimaryShard{"shard0"};
    const NamespaceString kNamespace = NamespaceString::createNamespaceString_forTest("db1.foo");

private:
    const HostAndPort configHost{"configHost1"};
    const ConnectionString configCS{ConnectionString::forReplicaSet("configReplSet", {configHost})};
    const HostAndPort clientHost{"clientHost1"};
};

// Direct tests for InitialSplitPolicy::createFirstChunks which is the base call for both the config
// server and shard server's shard collection logic
class CreateFirstChunksTest : public ShardCollectionTestBase {
protected:
    const ShardKeyPattern kShardKeyPattern{BSON("x" << 1)};
    const ShardKeyPattern kIdShardKeyPattern{BSON("_id" << 1)};
};

TEST_F(CreateFirstChunksTest, NonEmptyCollection_NoZones_OneChunkToPrimary) {
    const std::vector<ShardType> kShards{ShardType("shard0", "rs0/shard0:123"),
                                         ShardType("shard1", "rs1/shard1:123"),
                                         ShardType("shard2", "rs2/shard2:123")};

    const auto connStr = assertGet(ConnectionString::parse(kShards[1].getHost()));

    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(connStr);
    targeter->setFindHostReturnValue(connStr.getServers()[0]);
    targeterFactory()->addTargeterToReturn(connStr, std::move(targeter));

    setupShards(kShards);

    auto uuid = UUID::gen();
    {
        Lock::GlobalWrite lk(operationContext());
        CollectionCatalog::write(getServiceContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(operationContext(),
                                       std::make_shared<CollectionMock>(uuid, kNamespace),
                                       /*ts=*/boost::none);
        });
    }

    auto future = launchAsync([&] {
        ThreadClient tc("Test", getServiceContext()->getService());
        auto opCtx = cc().makeOperationContext();

        ShardsvrCreateCollectionRequest request;
        request.setPresplitHashedZones(false);
        auto optimization =
            create_collection_util::createPolicy(opCtx.get(),
                                                 kShardKeyPattern,
                                                 request.getPresplitHashedZones().value_or(false),
                                                 {}, /* tags */
                                                 3 /* numShards */,
                                                 false /* collectionIsEmpty */,
                                                 false /* unsplittable */,
                                                 boost::none /* dataShard */);
        return optimization->createFirstChunks(
            opCtx.get(), kShardKeyPattern, {uuid, ShardId("shard1")});
    });

    const auto& firstChunks = future.default_timed_get();
    ASSERT_EQ(1U, firstChunks.chunks.size());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[0].getShard());
}

TEST_F(CreateFirstChunksTest, NonEmptyCollection_WithZones_OneChunkToPrimary) {
    const std::vector<ShardType> kShards{ShardType("shard0", "rs0/shard0:123", {"TestZone"}),
                                         ShardType("shard1", "rs1/shard1:123", {"TestZone"}),
                                         ShardType("shard2", "rs2/shard2:123")};
    setupShards(kShards);

    std::vector<TagsType> zones{
        TagsType(kNamespace,
                 "TestZone",
                 ChunkRange(kShardKeyPattern.getKeyPattern().globalMin(), BSON("x" << 0)))};
    bool collectionIsEmpty = false;

    ShardsvrCreateCollectionRequest request;
    request.setPresplitHashedZones(false);
    auto optimization =
        create_collection_util::createPolicy(operationContext(),
                                             kShardKeyPattern,
                                             request.getPresplitHashedZones().value_or(false),
                                             std::move(zones),
                                             3 /* numShards */,
                                             collectionIsEmpty,
                                             false /* unsplittable */,
                                             boost::none /* dataShard */);

    const auto firstChunks = optimization->createFirstChunks(
        operationContext(), kShardKeyPattern, {UUID::gen(), ShardId("shard1")});

    ASSERT_EQ(1U, firstChunks.chunks.size());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[0].getShard());
}

TEST_F(CreateFirstChunksTest, EmptyCollection_NoSplitPoints_OneChunkToPrimary) {
    const std::vector<ShardType> kShards{ShardType("shard0", "rs0/shard0:123"),
                                         ShardType("shard1", "rs1/shard1:123"),
                                         ShardType("shard2", "rs2/shard2:123")};

    const auto connStr = assertGet(ConnectionString::parse(kShards[1].getHost()));

    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(connStr);
    targeter->setFindHostReturnValue(connStr.getServers()[0]);
    targeterFactory()->addTargeterToReturn(connStr, std::move(targeter));

    setupShards(kShards);

    auto future = launchAsync([&] {
        ThreadClient tc("Test", getServiceContext()->getService());
        auto opCtx = cc().makeOperationContext();

        bool collectionIsEmpty = true;
        bool isUnsplittable = false;

        ShardsvrCreateCollectionRequest request;
        request.setPresplitHashedZones(false);
        auto optimization =
            create_collection_util::createPolicy(opCtx.get(),
                                                 kShardKeyPattern,
                                                 request.getPresplitHashedZones().value_or(false),
                                                 {} /* tags */,
                                                 3 /* numShards */,
                                                 collectionIsEmpty,
                                                 isUnsplittable,
                                                 boost::none /* dataShard */);

        return optimization->createFirstChunks(
            opCtx.get(), kShardKeyPattern, {UUID::gen(), ShardId("shard1")});
    });

    const auto& firstChunks = future.default_timed_get();
    ASSERT_EQ(1U, firstChunks.chunks.size());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[0].getShard());
}

TEST_F(CreateFirstChunksTest, Unsplittable_OneChunkToPrimary) {
    const std::vector<ShardType> kShards{ShardType("shard0", "rs0/shard0:123"),
                                         ShardType("shard1", "rs1/shard1:123"),
                                         ShardType("shard2", "rs2/shard2:123")};

    const auto connStr = assertGet(ConnectionString::parse(kShards[1].getHost()));

    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(connStr);
    targeter->setFindHostReturnValue(connStr.getServers()[0]);
    targeterFactory()->addTargeterToReturn(connStr, std::move(targeter));

    setupShards(kShards);

    auto future = launchAsync([&] {
        ThreadClient tc("Test", getServiceContext()->getService());
        auto opCtx = cc().makeOperationContext();

        ShardsvrCreateCollectionRequest request;
        request.setPresplitHashedZones(false);
        auto optimization =
            create_collection_util::createPolicy(opCtx.get(),
                                                 kIdShardKeyPattern,
                                                 request.getPresplitHashedZones().value_or(false),
                                                 {} /* tags */,
                                                 3 /* numShards */,
                                                 true /*collectionIsEmpty*/,
                                                 true /* unsplittable */,
                                                 boost::none /* dataShard */);

        return optimization->createFirstChunks(
            opCtx.get(), kIdShardKeyPattern, {UUID::gen(), ShardId("shard1")});
    });

    const auto& firstChunks = future.default_timed_get();
    ASSERT_EQ(1U, firstChunks.chunks.size());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[0].getShard());
}

TEST_F(CreateFirstChunksTest, EmptyCollection_WithZones_ManyChunksOnFirstZoneShard) {
    const std::vector<ShardType> kShards{ShardType("shard0", "rs0/shard0:123", {"TestZone"}),
                                         ShardType("shard1", "rs1/shard1:123", {"TestZone"}),
                                         ShardType("shard2", "rs2/shard2:123")};
    setupShards(kShards);

    std::vector<TagsType> zones{
        TagsType(kNamespace,
                 "TestZone",
                 ChunkRange(kShardKeyPattern.getKeyPattern().globalMin(), BSON("x" << 0)))};
    bool collectionIsEmpty = true;
    ShardsvrCreateCollectionRequest request;
    request.setPresplitHashedZones(false);
    auto optimization =
        create_collection_util::createPolicy(operationContext(),
                                             kShardKeyPattern,
                                             request.getPresplitHashedZones().value_or(false),
                                             std::move(zones),
                                             3 /* numShards */,
                                             collectionIsEmpty,
                                             false /* unsplittable */,
                                             boost::none /* dataShard */);

    const auto firstChunks = optimization->createFirstChunks(
        operationContext(), kShardKeyPattern, {UUID::gen(), ShardId("shard1")});

    ASSERT_EQ(2U, firstChunks.chunks.size());
    ASSERT_EQ(ChunkRange(kShardKeyPattern.getKeyPattern().globalMin(), BSON("x" << 0)),
              firstChunks.chunks[0].getRange());
    ASSERT_EQ(ChunkRange(BSON("x" << 0), kShardKeyPattern.getKeyPattern().globalMax()),
              firstChunks.chunks[1].getRange());

    ASSERT_EQ(kShards[0].getName(), firstChunks.chunks[0].getShard());
    // Chunk1 (no zone) goes to any shard (selected randomly, round-robin);
}

}  // namespace
}  // namespace mongo
