
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
                           const BSONArray& splitPoints) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(shardHost, request.target);
            string cmdName = request.cmdObj.firstElement().fieldName();
            ASSERT_EQUALS("autoSplitVector", cmdName);
            // autoSplitVector concatenates the collection name to the command's db
            const auto receivedNs =
                request.dbname + '.' + request.cmdObj["autoSplitVector"].String();
            ASSERT_EQUALS(kNamespace.ns(), receivedNs);

            ASSERT_BSONOBJ_EQ(keyPattern.toBSON(), request.cmdObj["keyPattern"].Obj());
            ASSERT_BSONOBJ_EQ(keyPattern.getKeyPattern().globalMin(), request.cmdObj["min"].Obj());
            ASSERT_BSONOBJ_EQ(keyPattern.getKeyPattern().globalMax(), request.cmdObj["max"].Obj());
            ASSERT_EQUALS(64 * 1024 * 1024ULL,
                          static_cast<uint64_t>(request.cmdObj["maxChunkSizeBytes"].numberLong()));

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


// Tests which exercise the ShardingCatalogManager::shardCollection logic, which is what the config
// server uses to shard collections, when the '_shardsvrShardCollection' command is not available
// (fast initial split optimization)
class ConfigServerShardCollectionTest : public ShardCollectionTestBase {
protected:
    void checkWrittenChunks(const std::vector<ChunkType>& expectedChunks) {
        const auto grid = Grid::get(operationContext());
        const auto catalogClient = grid->catalogClient();
        repl::OpTime unusedOpTime;
        const auto writtenChunks =
            assertGet(catalogClient->getChunks(operationContext(),
                                               BSON("ns" << kNamespace.ns()),
                                               BSON("min" << 1),
                                               boost::none,
                                               &unusedOpTime,
                                               repl::ReadConcernLevel::kLocalReadConcern));
        ASSERT_EQ(expectedChunks.size(), writtenChunks.size());

        auto itE = expectedChunks.begin();
        auto itW = writtenChunks.begin();
        for (; itE != expectedChunks.end(); itE++, itW++) {
            const auto& expected = *itE;
            const auto& written = *itW;
            ASSERT_BSONOBJ_EQ(expected.getMin(), expected.getMin());
            ASSERT_BSONOBJ_EQ(expected.getMax(), expected.getMax());
            ASSERT_EQ(expected.getShard(), written.getShard());
        }
    }

    const ShardKeyPattern keyPattern{BSON("_id" << 1)};
    const BSONObj defaultCollation;
};

TEST_F(ConfigServerShardCollectionTest, Partially_Written_Chunks_Present) {
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shardHost");
    setupShards(vector<ShardType>{shard});

    setupDatabase(kNamespace.db().toString(), shard.getName(), true);

    // Set up chunks in the collection, indicating that another mongos must have already started
    // sharding the collection.
    ChunkType chunk;
    chunk.setNS(kNamespace);
    chunk.setVersion(ChunkVersion(2, 0, OID::gen()));
    chunk.setShard(shard.getName());
    chunk.setMin(BSON("_id" << 1));
    chunk.setMax(BSON("_id" << 5));
    setupChunks({chunk});

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->shardCollection(operationContext(),
                                             kNamespace,
                                             boost::none,  // UUID
                                             keyPattern,
                                             defaultCollation,
                                             false,
                                             {},
                                             false,  // isFromMapReduce
                                             testPrimaryShard),
                       AssertionException,
                       ErrorCodes::ManualInterventionRequired);
}

TEST_F(ConfigServerShardCollectionTest, RangeSharding_ForMapReduce_NoInitialSplitPoints) {
    const HostAndPort shardHost{"shardHost"};
    ShardType shard;
    shard.setName("shard0");
    shard.setHost(shardHost.toString());

    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(shardHost));
    targeter->setFindHostReturnValue(shardHost);
    targeterFactory()->addTargeterToReturn(ConnectionString(shardHost), std::move(targeter));

    setupShards(vector<ShardType>{shard});

    setupDatabase(kNamespace.db().toString(), shard.getName(), true);

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(operationContext())
            ->shardCollection(opCtx.get(),
                              kNamespace,
                              boost::none,  // UUID
                              keyPattern,
                              defaultCollation,
                              false,
                              {},    // No split points
                              true,  // isFromMapReduce
                              testPrimaryShard);
    });

    // Expect the set shard version for that namespace.
    // We do not check for a specific ChunkVersion, because we cannot easily know the OID that was
    // generated by shardCollection for the first chunk.
    // TODO SERVER-29451: add hooks to the mock storage engine to expect reads and writes.
    expectSetShardVersion(shardHost, shard, kNamespace, boost::none /* expected ChunkVersion */);

    future.default_timed_get();

    checkWrittenChunks(
        {ChunkType(kNamespace,
                   {keyPattern.getKeyPattern().globalMin(), keyPattern.getKeyPattern().globalMax()},
                   ChunkVersion::IGNORED(),
                   testPrimaryShard)});
}

TEST_F(ConfigServerShardCollectionTest, RangeSharding_ForMapReduce_WithInitialSplitPoints) {
    const HostAndPort shard0Host{"shardHost0"};
    const HostAndPort shard1Host{"shardHost1"};
    const HostAndPort shard2Host{"shardHost2"};

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost(shard0Host.toString());

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost(shard1Host.toString());

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost(shard2Host.toString());

    std::unique_ptr<RemoteCommandTargeterMock> targeter0(
        stdx::make_unique<RemoteCommandTargeterMock>());
    std::unique_ptr<RemoteCommandTargeterMock> targeter1(
        stdx::make_unique<RemoteCommandTargeterMock>());
    std::unique_ptr<RemoteCommandTargeterMock> targeter2(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter0->setConnectionStringReturnValue(ConnectionString(shard0Host));
    targeter0->setFindHostReturnValue(shard0Host);
    targeterFactory()->addTargeterToReturn(ConnectionString(shard0Host), std::move(targeter0));
    targeter1->setConnectionStringReturnValue(ConnectionString(shard1Host));
    targeter1->setFindHostReturnValue(shard1Host);
    targeterFactory()->addTargeterToReturn(ConnectionString(shard1Host), std::move(targeter1));
    targeter2->setConnectionStringReturnValue(ConnectionString(shard2Host));
    targeter2->setFindHostReturnValue(shard2Host);
    targeterFactory()->addTargeterToReturn(ConnectionString(shard2Host), std::move(targeter2));

    setupShards(vector<ShardType>{shard0, shard1, shard2});

    setupDatabase(kNamespace.db().toString(), shard0.getName(), true);

    BSONObj splitPoint0 = BSON("_id" << 1);
    BSONObj splitPoint1 = BSON("_id" << 100);
    BSONObj splitPoint2 = BSON("_id" << 200);
    BSONObj splitPoint3 = BSON("_id" << 300);

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(operationContext())
            ->shardCollection(opCtx.get(),
                              kNamespace,
                              boost::none,  // UUID
                              keyPattern,
                              defaultCollation,
                              true,
                              {splitPoint0, splitPoint1, splitPoint2, splitPoint3},
                              true,  // isFromMapReduce
                              testPrimaryShard);
    });

    // Expect the set shard version for that namespace
    // We do not check for a specific ChunkVersion, because we cannot easily know the OID that was
    // generated by shardCollection for the first chunk.
    // TODO SERVER-29451: add hooks to the mock storage engine to expect reads and writes.
    expectSetShardVersion(shard0Host, shard0, kNamespace, boost::none /* expected ChunkVersion */);

    future.default_timed_get();

    checkWrittenChunks({ChunkType(kNamespace,
                                  ChunkRange{keyPattern.getKeyPattern().globalMin(), splitPoint0},
                                  ChunkVersion::IGNORED(),
                                  shard0.getName()),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint0, splitPoint1},
                                  ChunkVersion::IGNORED(),
                                  shard1.getName()),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint1, splitPoint2},
                                  ChunkVersion::IGNORED(),
                                  shard2.getName()),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint2, splitPoint3},
                                  ChunkVersion::IGNORED(),
                                  shard0.getName()),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint3, keyPattern.getKeyPattern().globalMax()},
                                  ChunkVersion::IGNORED(),
                                  shard1.getName())});
}

TEST_F(ConfigServerShardCollectionTest, RangeSharding_NoInitialSplitPoints_NoSplitVectorPoints) {
    const HostAndPort shardHost{"shardHost"};
    ShardType shard;
    shard.setName("shard0");
    shard.setHost(shardHost.toString());

    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(shardHost));
    targeter->setFindHostReturnValue(shardHost);
    targeterFactory()->addTargeterToReturn(ConnectionString(shardHost), std::move(targeter));

    setupShards(vector<ShardType>{shard});

    setupDatabase(kNamespace.db().toString(), shard.getName(), true);

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(operationContext())
            ->shardCollection(opCtx.get(),
                              kNamespace,
                              boost::none,  // UUID
                              keyPattern,
                              defaultCollation,
                              false,
                              {},     // No split points
                              false,  // isFromMapReduce
                              testPrimaryShard);
    });

    // Respond to the splitVector command sent to the shard to figure out initial split points.
    expectSplitVector(shardHost, keyPattern, BSONArray());

    // Expect the set shard version for that namespace.
    // We do not check for a specific ChunkVersion, because we cannot easily know the OID that was
    // generated by shardCollection for the first chunk.
    // TODO SERVER-29451: add hooks to the mock storage engine to expect reads and writes.
    expectSetShardVersion(shardHost, shard, kNamespace, boost::none /* expected ChunkVersion */);

    future.default_timed_get();

    checkWrittenChunks(
        {ChunkType(kNamespace,
                   {keyPattern.getKeyPattern().globalMin(), keyPattern.getKeyPattern().globalMax()},
                   ChunkVersion::IGNORED(),
                   testPrimaryShard)});
}

TEST_F(ConfigServerShardCollectionTest, RangeSharding_NoInitialSplitPoints_WithSplitVectorPoints) {
    const HostAndPort shardHost{"shardHost"};
    ShardType shard;
    shard.setName("shard0");
    shard.setHost(shardHost.toString());

    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(shardHost));
    targeter->setFindHostReturnValue(shardHost);
    targeterFactory()->addTargeterToReturn(ConnectionString(shardHost), std::move(targeter));

    setupShards(vector<ShardType>{shard});

    setupDatabase(kNamespace.db().toString(), shard.getName(), true);

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(operationContext())
            ->shardCollection(opCtx.get(),
                              kNamespace,
                              boost::none,  // UUID
                              keyPattern,
                              defaultCollation,
                              false,
                              {},     // No split points
                              false,  // isFromMapReduce
                              testPrimaryShard);
    });

    BSONObj splitPoint0 = BSON("_id" << 1);
    BSONObj splitPoint1 = BSON("_id" << 100);
    BSONObj splitPoint2 = BSON("_id" << 200);
    BSONObj splitPoint3 = BSON("_id" << 300);

    // Respond to the splitVector command sent to the shard to figure out initial split points.
    expectSplitVector(shardHost,
                      keyPattern,
                      BSON_ARRAY(splitPoint0 << splitPoint1 << splitPoint2 << splitPoint3));

    // Expect the set shard version for that namespace
    // We do not check for a specific ChunkVersion, because we cannot easily know the OID that was
    // generated by shardCollection for the first chunk.
    // TODO SERVER-29451: add hooks to the mock storage engine to expect reads and writes.
    expectSetShardVersion(shardHost, shard, kNamespace, boost::none);

    future.default_timed_get();

    checkWrittenChunks({ChunkType(kNamespace,
                                  ChunkRange{keyPattern.getKeyPattern().globalMin(), splitPoint0},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint0, splitPoint1},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint1, splitPoint2},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint2, splitPoint3},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint3, keyPattern.getKeyPattern().globalMax()},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard)});
}

TEST_F(ConfigServerShardCollectionTest, RangeSharding_WithInitialSplitPoints_NoSplitVectorPoints) {
    const HostAndPort shardHost{"shardHost"};
    ShardType shard;
    shard.setName("shard0");
    shard.setHost(shardHost.toString());

    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(shardHost));
    targeter->setFindHostReturnValue(shardHost);
    targeterFactory()->addTargeterToReturn(ConnectionString(shardHost), std::move(targeter));

    setupShards(vector<ShardType>{shard});

    setupDatabase(kNamespace.db().toString(), shard.getName(), true);

    BSONObj splitPoint0 = BSON("_id" << 1);
    BSONObj splitPoint1 = BSON("_id" << 100);
    BSONObj splitPoint2 = BSON("_id" << 200);
    BSONObj splitPoint3 = BSON("_id" << 300);

    // Now start actually sharding the collection.
    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(operationContext())
            ->shardCollection(opCtx.get(),
                              kNamespace,
                              boost::none,  // UUID
                              keyPattern,
                              defaultCollation,
                              false,
                              {splitPoint0, splitPoint1, splitPoint2, splitPoint3},
                              false,  // isFromMapReduce
                              testPrimaryShard);
    });

    // Expect the set shard version for that namespace
    // We do not check for a specific ChunkVersion, because we cannot easily know the OID that was
    // generated by shardCollection for the first chunk.
    // TODO SERVER-29451: add hooks to the mock storage engine to expect reads and writes.
    expectSetShardVersion(shardHost, shard, kNamespace, boost::none);

    future.default_timed_get();

    checkWrittenChunks({ChunkType(kNamespace,
                                  ChunkRange{keyPattern.getKeyPattern().globalMin(), splitPoint0},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint0, splitPoint1},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint1, splitPoint2},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint2, splitPoint3},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard),
                        ChunkType(kNamespace,
                                  ChunkRange{splitPoint3, keyPattern.getKeyPattern().globalMax()},
                                  ChunkVersion::IGNORED(),
                                  testPrimaryShard)});
}


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
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(connStr);
    targeter->setFindHostReturnValue(connStr.getServers()[0]);
    targeterFactory()->addTargeterToReturn(connStr, std::move(targeter));

    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
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
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(connStr);
    targeter->setFindHostReturnValue(connStr.getServers()[0]);
    targeterFactory()->addTargeterToReturn(connStr, std::move(targeter));

    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
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
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(connStr);
    targeter->setFindHostReturnValue(connStr.getServers()[0]);
    targeterFactory()->addTargeterToReturn(connStr, std::move(targeter));

    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
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
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(connStr);
    targeter->setFindHostReturnValue(connStr.getServers()[0]);
    targeterFactory()->addTargeterToReturn(connStr, std::move(targeter));

    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
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
