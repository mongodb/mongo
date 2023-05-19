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


#include <vector>

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/shard_key_pattern.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using executor::RemoteCommandRequest;
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
    shardRegistry()->reload(operationContext());

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
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        CreateCollectionRequest request;
        request.setNumInitialChunks(0);
        request.setPresplitHashedZones(false);
        auto optimization = InitialSplitPolicy::calculateOptimizationStrategy(
            operationContext(),
            kShardKeyPattern,
            request.getNumInitialChunks().value(),
            request.getPresplitHashedZones().value(),
            {}, /* tags */
            3 /* numShards */,
            false /* collectionIsEmpty */);
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
    shardRegistry()->reload(operationContext());

    std::vector<TagsType> zones{
        TagsType(kNamespace,
                 "TestZone",
                 ChunkRange(kShardKeyPattern.getKeyPattern().globalMin(), BSON("x" << 0)))};
    bool collectionIsEmpty = false;

    CreateCollectionRequest request;
    request.setNumInitialChunks(0);
    request.setPresplitHashedZones(false);
    auto optimization =
        InitialSplitPolicy::calculateOptimizationStrategy(operationContext(),
                                                          kShardKeyPattern,
                                                          request.getNumInitialChunks().value(),
                                                          request.getPresplitHashedZones().value(),
                                                          zones,
                                                          3 /* numShards */,
                                                          collectionIsEmpty);
    ASSERT(optimization->isOptimized());

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
    shardRegistry()->reload(operationContext());

    auto future = launchAsync([&] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        std::vector<TagsType> zones{};
        bool collectionIsEmpty = true;

        CreateCollectionRequest request;
        request.setNumInitialChunks(0);
        request.setPresplitHashedZones(false);
        auto optimization = InitialSplitPolicy::calculateOptimizationStrategy(
            operationContext(),
            kShardKeyPattern,
            request.getNumInitialChunks().value(),
            request.getPresplitHashedZones().value(),
            zones,
            3 /* numShards */,
            collectionIsEmpty);
        ASSERT(optimization->isOptimized());

        return optimization->createFirstChunks(
            operationContext(), kShardKeyPattern, {UUID::gen(), ShardId("shard1")});
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
    shardRegistry()->reload(operationContext());

    std::vector<TagsType> zones{
        TagsType(kNamespace,
                 "TestZone",
                 ChunkRange(kShardKeyPattern.getKeyPattern().globalMin(), BSON("x" << 0)))};
    bool collectionIsEmpty = true;
    CreateCollectionRequest request;
    request.setNumInitialChunks(0);
    request.setPresplitHashedZones(false);
    auto optimization =
        InitialSplitPolicy::calculateOptimizationStrategy(operationContext(),
                                                          kShardKeyPattern,
                                                          request.getNumInitialChunks().value(),
                                                          request.getPresplitHashedZones().value(),
                                                          zones,
                                                          3 /* numShards */,
                                                          collectionIsEmpty);
    ASSERT(optimization->isOptimized());

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
