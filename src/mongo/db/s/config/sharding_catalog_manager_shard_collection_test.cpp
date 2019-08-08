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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <set>
#include <string>
#include <vector>

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using std::set;
using std::string;
using std::vector;
using unittest::assertGet;

class ShardCollectionTestBase : public ConfigServerTestFixture {
protected:
    void expectSplitVector(const HostAndPort& shardHost,
                           const ShardKeyPattern& keyPattern,
                           const BSONObj& splitPoints) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(shardHost, request.target);
            string cmdName = request.cmdObj.firstElement().fieldName();
            ASSERT_EQUALS("splitVector", cmdName);
            ASSERT_EQUALS(kNamespace.ns(),
                          request.cmdObj["splitVector"].String());  // splitVector uses full ns

            ASSERT_BSONOBJ_EQ(keyPattern.toBSON(), request.cmdObj["keyPattern"].Obj());
            ASSERT_BSONOBJ_EQ(keyPattern.getKeyPattern().globalMin(), request.cmdObj["min"].Obj());
            ASSERT_BSONOBJ_EQ(keyPattern.getKeyPattern().globalMax(), request.cmdObj["max"].Obj());
            ASSERT_EQUALS(64 * 1024 * 1024ULL,
                          static_cast<uint64_t>(request.cmdObj["maxChunkSizeBytes"].numberLong()));
            ASSERT_EQUALS(0, request.cmdObj["maxSplitPoints"].numberLong());
            ASSERT_EQUALS(0, request.cmdObj["maxChunkObjects"].numberLong());

            ASSERT_BSONOBJ_EQ(
                ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(),
                rpc::TrackingMetadata::removeTrackingData(request.metadata));

            return BSON("ok" << 1 << "splitKeys" << splitPoints);
        });
    }

    const ShardId testPrimaryShard{"shard0"};
    const NamespaceString kNamespace{"db1.foo"};

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

TEST_F(CreateFirstChunksTest, Split_Disallowed_With_Both_SplitPoints_And_Zones) {
    ASSERT_THROWS_CODE(
        InitialSplitPolicy::createFirstChunksOptimized(
            operationContext(),
            kNamespace,
            kShardKeyPattern,
            ShardId("shard1"),
            {BSON("x" << 0)},
            {TagsType(kNamespace,
                      "TestZone",
                      ChunkRange(kShardKeyPattern.getKeyPattern().globalMin(), BSON("x" << 0)))},
            InitialSplitPolicy::ShardingOptimizationType::SplitPointsProvided,
            true /* isEmpty */),
        AssertionException,
        ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(
        InitialSplitPolicy::createFirstChunksOptimized(
            operationContext(),
            kNamespace,
            kShardKeyPattern,
            ShardId("shard1"),
            {BSON("x" << 0)}, /* No split points */
            {TagsType(kNamespace,
                      "TestZone",
                      ChunkRange(kShardKeyPattern.getKeyPattern().globalMin(), BSON("x" << 0)))},
            InitialSplitPolicy::ShardingOptimizationType::TagsProvidedWithEmptyCollection,
            false /* isEmpty */),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

TEST_F(CreateFirstChunksTest, NonEmptyCollection_SplitPoints_FromSplitVector_ManyChunksToPrimary) {
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

        auto optimization =
            InitialSplitPolicy::calculateOptimizationType({}, /* splitPoints */
                                                          {}, /* tags */
                                                          false /* collectionIsEmpty */);

        ASSERT_EQ(optimization, InitialSplitPolicy::ShardingOptimizationType::None);
        return InitialSplitPolicy::createFirstChunksUnoptimized(
            opCtx.get(), kNamespace, kShardKeyPattern, ShardId("shard1"));
    });

    expectSplitVector(connStr.getServers()[0], kShardKeyPattern, BSON_ARRAY(BSON("x" << 0)));

    const auto& firstChunks = future.default_timed_get();
    ASSERT_EQ(2U, firstChunks.chunks.size());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[0].getShard());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[1].getShard());
}

TEST_F(CreateFirstChunksTest, NonEmptyCollection_SplitPoints_FromClient_ManyChunksToPrimary) {
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

        std::vector<BSONObj> splitPoints{BSON("x" << 0)};
        std::vector<TagsType> zones{};
        bool collectionIsEmpty = false;

        auto optimization =
            InitialSplitPolicy::calculateOptimizationType(splitPoints, zones, collectionIsEmpty);
        ASSERT_EQ(optimization, InitialSplitPolicy::ShardingOptimizationType::SplitPointsProvided);

        return InitialSplitPolicy::createFirstChunksOptimized(opCtx.get(),
                                                              kNamespace,
                                                              kShardKeyPattern,
                                                              ShardId("shard1"),
                                                              splitPoints,
                                                              zones,
                                                              optimization,
                                                              collectionIsEmpty);
    });

    const auto& firstChunks = future.default_timed_get();
    ASSERT_EQ(2U, firstChunks.chunks.size());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[0].getShard());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[1].getShard());
}

TEST_F(CreateFirstChunksTest, NonEmptyCollection_WithZones_OneChunkToPrimary) {
    const std::vector<ShardType> kShards{ShardType("shard0", "rs0/shard0:123", {"TestZone"}),
                                         ShardType("shard1", "rs1/shard1:123", {"TestZone"}),
                                         ShardType("shard2", "rs2/shard2:123")};
    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    std::vector<BSONObj> splitPoints{};
    std::vector<TagsType> zones{
        TagsType(kNamespace,
                 "TestZone",
                 ChunkRange(kShardKeyPattern.getKeyPattern().globalMin(), BSON("x" << 0)))};
    bool collectionIsEmpty = false;

    auto optimization =
        InitialSplitPolicy::calculateOptimizationType(splitPoints, zones, collectionIsEmpty);
    ASSERT_EQ(optimization,
              InitialSplitPolicy::ShardingOptimizationType::TagsProvidedWithNonEmptyCollection);

    const auto firstChunks = InitialSplitPolicy::createFirstChunksOptimized(operationContext(),
                                                                            kNamespace,
                                                                            kShardKeyPattern,
                                                                            ShardId("shard1"),
                                                                            splitPoints,
                                                                            zones,
                                                                            optimization,
                                                                            collectionIsEmpty);

    ASSERT_EQ(1U, firstChunks.chunks.size());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[0].getShard());
}

TEST_F(CreateFirstChunksTest, EmptyCollection_SplitPoints_FromClient_ManyChunksDistributed) {
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

        std::vector<BSONObj> splitPoints{BSON("x" << 0), BSON("x" << 100)};
        std::vector<TagsType> zones{};
        bool collectionIsEmpty = true;

        auto optimization =
            InitialSplitPolicy::calculateOptimizationType(splitPoints, zones, collectionIsEmpty);
        ASSERT_EQ(optimization, InitialSplitPolicy::ShardingOptimizationType::SplitPointsProvided);

        return InitialSplitPolicy::createFirstChunksOptimized(opCtx.get(),
                                                              kNamespace,
                                                              kShardKeyPattern,
                                                              ShardId("shard1"),
                                                              splitPoints,
                                                              zones,
                                                              optimization,
                                                              collectionIsEmpty);
    });

    const auto& firstChunks = future.default_timed_get();
    ASSERT_EQ(3U, firstChunks.chunks.size());
    ASSERT_EQ(kShards[0].getName(), firstChunks.chunks[0].getShard());
    ASSERT_EQ(kShards[1].getName(), firstChunks.chunks[1].getShard());
    ASSERT_EQ(kShards[2].getName(), firstChunks.chunks[2].getShard());
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

        std::vector<BSONObj> splitPoints{};
        std::vector<TagsType> zones{};
        bool collectionIsEmpty = true;

        auto optimization =
            InitialSplitPolicy::calculateOptimizationType(splitPoints, zones, collectionIsEmpty);
        ASSERT_EQ(optimization, InitialSplitPolicy::ShardingOptimizationType::EmptyCollection);

        return InitialSplitPolicy::createFirstChunksOptimized(opCtx.get(),
                                                              kNamespace,
                                                              kShardKeyPattern,
                                                              ShardId("shard1"),
                                                              splitPoints,
                                                              zones,
                                                              optimization,
                                                              collectionIsEmpty);
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

    std::vector<BSONObj> splitPoints{};
    std::vector<TagsType> zones{
        TagsType(kNamespace,
                 "TestZone",
                 ChunkRange(kShardKeyPattern.getKeyPattern().globalMin(), BSON("x" << 0)))};
    bool collectionIsEmpty = true;

    auto optimization =
        InitialSplitPolicy::calculateOptimizationType(splitPoints, zones, collectionIsEmpty);
    ASSERT_EQ(optimization,
              InitialSplitPolicy::ShardingOptimizationType::TagsProvidedWithEmptyCollection);

    const auto firstChunks = InitialSplitPolicy::createFirstChunksOptimized(operationContext(),
                                                                            kNamespace,
                                                                            kShardKeyPattern,
                                                                            ShardId("shard1"),
                                                                            splitPoints,
                                                                            zones,
                                                                            optimization,
                                                                            collectionIsEmpty);

    ASSERT_EQ(2U, firstChunks.chunks.size());
    ASSERT_EQ(kShards[0].getName(), firstChunks.chunks[0].getShard());
    ASSERT_EQ(kShards[0].getName(), firstChunks.chunks[1].getShard());
}

}  // namespace
}  // namespace mongo
