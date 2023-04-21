/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/commands.h"
#include "mongo/db/s/balancer/balancer_chunk_selection_policy_impl.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/db/s/balancer/migration_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/request_types/get_stats_for_balancing_gen.h"
#include "mongo/s/type_collection_common_types_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const std::string kDbName = "TestDb";
const auto kNamespace = NamespaceString::createNamespaceString_forTest(kDbName, "TestColl");
const int kSizeOnDisk = 1;

class BalancerChunkSelectionTest : public MigrationTestFixture {
protected:
    BalancerChunkSelectionTest()
        : _clusterStats(std::make_unique<ClusterStatisticsImpl>()),
          _chunkSelectionPolicy(
              std::make_unique<BalancerChunkSelectionPolicyImpl>(_clusterStats.get())) {}

    /**
     * Generates a default chunks distribution across shards with the form:
     * [MinKey, 0), [0, 1), [1, 2) ... [N - 2, MaxKey)
     */
    std::map<ShardId, std::vector<ChunkRange>> generateDefaultChunkRanges(
        const std::vector<ShardId>& shards) {

        std::map<ShardId, std::vector<ChunkRange>> chunksPerShard;
        for (auto i = 0U; i < shards.size(); ++i) {
            const ShardId& shardId = shards[i];
            const auto min = (i == 0 ? kKeyPattern.globalMin() : BSON(kPattern << int(i - 1)));
            const auto max =
                (i == shards.size() - 1 ? kKeyPattern.globalMax() : BSON(kPattern << int(i)));
            chunksPerShard[shardId].push_back(ChunkRange(min, max));
        }
        return chunksPerShard;
    }

    /**
     * Sets up mock network to expect a listDatabases command and returns a BSON response with
     * a dummy sizeOnDisk.
     */
    void expectListDatabasesCommand() {
        BSONObjBuilder resultBuilder;
        CommandHelpers::appendCommandStatusNoThrow(resultBuilder, Status::OK());

        onCommand([&resultBuilder](const RemoteCommandRequest& request) {
            ASSERT(request.cmdObj["listDatabases"]);
            std::vector<BSONObj> dbInfos;
            BSONObjBuilder b;
            b.append("name", kDbName);
            b.append("sizeOnDisk", kSizeOnDisk);
            b.append("empty", kSizeOnDisk > 0);
            resultBuilder.append("databases", dbInfos);
            resultBuilder.append("totalSize", kSizeOnDisk);
            return resultBuilder.obj();
        });
    }

    /**
     * Sets up mock network to expect a serverStatus command and returns a BSON response with
     * a dummy version.
     */
    void expectServerStatusCommand() {
        BSONObjBuilder resultBuilder;
        CommandHelpers::appendCommandStatusNoThrow(resultBuilder, Status::OK());

        onCommand([&resultBuilder](const RemoteCommandRequest& request) {
            ASSERT(request.cmdObj["serverStatus"]);
            resultBuilder.append("version", "MONGO_VERSION");
            return resultBuilder.obj();
        });
    }

    /**
     * Sets up mock network for all the shards to expect the commands executed for computing cluster
     * stats, which include listDatabase and serverStatus.
     */
    void expectGetStatsCommands(int numShards) {
        for (int i = 0; i < numShards; i++) {
            expectListDatabasesCommand();
            expectServerStatusCommand();
        }
    }

    /**
     * Sets up mock network to expect a _shardsvrGetStatsForBalancing command and returns a BSON
     * response with a dummy version.
     */
    void expectGetStatsForBalancingCommand() {
        BSONObjBuilder resultBuilder;
        CommandHelpers::appendCommandStatusNoThrow(resultBuilder, Status::OK());

        onCommand([&resultBuilder](const RemoteCommandRequest& request) {
            ASSERT(request.cmdObj[ShardsvrGetStatsForBalancing::kCommandName]);

            ShardsvrGetStatsForBalancingReply reply({CollStatsForBalancing(kNamespace, 12345)});
            reply.serialize(&resultBuilder);
            return resultBuilder.obj();
        });
    }

    /**
     * Sets up mock network for all the shards to expect the command `_shardsvrGetStatsForBalancing`
     * Given a request sent to a specific shard with below structure ...
     * {
     *     "_shardsvrGetStatsForBalancing" : 1,
     *     "collections" : [
     *         {
     *             "ns" : "TestDb.TestColl",
     *             "UUID" : "xxxx"
     *         },
     *         ...
     *     ]
     * }
     *
     * ... mocks a reply with the following structure:
     * {
     *     "stats" : [
     *         {
     *            "namespace" : "TestDb.TestColl",
     *            "collSize" : 12345,
     *         },
     *         ...
     *     ]
     * }
     */
    void expectGetStatsForBalancingCommands(const std::map<ShardId, int64_t>& collSizePerShard) {
        const auto& numShards = collSizePerShard.size();
        for (auto i = 0U; i < numShards; ++i) {
            BSONObjBuilder resultBuilder;
            CommandHelpers::appendCommandStatusNoThrow(resultBuilder, Status::OK());

            // Build a response for every given request
            onCommand([&](const RemoteCommandRequest& request) {
                ASSERT(request.cmdObj[ShardsvrGetStatsForBalancing::kCommandName]);

                // Get `shardId`
                ShardId shardId = getShardIdByHost(request.target);
                resultBuilder.append("shardId", shardId);

                // Build `stats` array: [ {"namespace": <nss>, "collSize": <collSize>}, ...]
                {
                    BSONArrayBuilder statsArrayBuilder(resultBuilder.subarrayStart("stats"));

                    ASSERT_EQ(1, collSizePerShard.count(shardId));
                    const auto& collSize = collSizePerShard.at(shardId);

                    for (const auto& reqColl :
                         request.cmdObj[ShardsvrGetStatsForBalancing::kCollectionsFieldName]
                             .Array()) {
                        const auto nss =
                            NamespaceWithOptionalUUID::parse(
                                IDLParserContext("BalancerChunkSelectionPolicyTest"), reqColl.Obj())
                                .getNs();

                        statsArrayBuilder.append(CollStatsForBalancing(nss, collSize).toBSON());
                    }
                }
                return resultBuilder.obj();
            });
        }
    }

    /**
     * Same as expectGetStatsForBalancingCommands with the difference that this function will expect
     * only one migration between the specified shards
     */
    void expectGetStatsForBalancingCommandsWithOneMigration(uint32_t numShards,
                                                            ShardId donorShardId,
                                                            ShardId recipientShardId) {
        ASSERT_NE(donorShardId, recipientShardId);

        const auto maxChunkSizeBytes =
            Grid::get(operationContext())->getBalancerConfiguration()->getMaxChunkSizeBytes();
        const auto defaultCollSizeOnShard = 2 * maxChunkSizeBytes;
        const auto imbalancedCollSizeOnRecipient = maxChunkSizeBytes;
        const auto imbalancedCollSizeOnDonor = 5 * maxChunkSizeBytes;

        for (auto i = 0U; i < numShards; ++i) {
            BSONObjBuilder resultBuilder;
            CommandHelpers::appendCommandStatusNoThrow(resultBuilder, Status::OK());

            // Build a response for every given request
            onCommand([&](const RemoteCommandRequest& request) {
                ASSERT(request.cmdObj[ShardsvrGetStatsForBalancing::kCommandName]);

                // Get `shardId`
                ShardId shardId = getShardIdByHost(request.target);
                resultBuilder.append("shardId", shardId);

                // Build `stats` array: [ {"namespace": <nss>, "collSize": <collSize>}, ...]
                {
                    bool firstColl = true;
                    BSONArrayBuilder statsArrayBuilder(resultBuilder.subarrayStart("stats"));
                    for (const auto& reqColl :
                         request.cmdObj[ShardsvrGetStatsForBalancing::kCollectionsFieldName]
                             .Array()) {
                        const auto nss =
                            NamespaceWithOptionalUUID::parse(
                                IDLParserContext("BalancerChunkSelectionPolicyTest"), reqColl.Obj())
                                .getNs();

                        const auto collSize = [&]() {
                            if (firstColl && shardId == donorShardId) {
                                return imbalancedCollSizeOnDonor;
                            } else if (firstColl && shardId == recipientShardId) {
                                return imbalancedCollSizeOnRecipient;
                            }
                            return defaultCollSizeOnShard;
                        }();

                        statsArrayBuilder.append(CollStatsForBalancing(nss, collSize).toBSON());
                        firstColl = false;
                    }
                }
                return resultBuilder.obj();
            });
        }
    }

    /**
     * Sets up a collection and its chunks according to the given range distribution across
     * shards
     */
    UUID setUpCollectionWithChunks(
        const NamespaceString& ns,
        const std::map<ShardId, std::vector<ChunkRange>>& chunksPerShard) {
        const UUID collUuid = UUID::gen();
        ChunkVersion version({OID::gen(), Timestamp(42)}, {2, 0});

        for (const auto& [shardId, chunkRanges] : chunksPerShard) {
            for (const auto& chunkRange : chunkRanges) {
                setUpChunk(collUuid, chunkRange.getMin(), chunkRange.getMax(), shardId, version);
                version.incMinor();
            }
            version.incMajor();
        }

        setUpCollection(ns, collUuid, version);

        return collUuid;
    }

    /**
     * Returns a new ShardType object with the specified zones appended to the given shard
     */
    ShardType appendZones(const ShardType& shard, std::vector<std::string> zones) {
        auto alteredShard = shard;
        alteredShard.setTags(zones);
        return alteredShard;
    }

    std::vector<ClusterStatistics::ShardStatistics> getShardStats(OperationContext* opCtx) {
        return uassertStatusOK(_clusterStats.get()->getStats(opCtx));
    }

    stdx::unordered_set<ShardId> getAllShardIds(OperationContext* opCtx) {
        const auto& shards = shardRegistry()->getAllShardIds(opCtx);
        return stdx::unordered_set<ShardId>(shards.begin(), shards.end());
    }

    /**
     * Syntactic sugar function for calling BalancerChunkSelectionPolicy::selectChunksToMove()
     */
    MigrateInfoVector selectChunksToMove(OperationContext* opCtx) {
        auto availableShards = getAllShardIds(opCtx);

        const auto& swChunksToMove = _chunkSelectionPolicy.get()->selectChunksToMove(
            opCtx, getShardStats(opCtx), &availableShards, &_imbalancedCollectionsCache);
        ASSERT_OK(swChunksToMove.getStatus());

        return swChunksToMove.getValue();
    }

    std::unique_ptr<ClusterStatistics> _clusterStats;
    stdx::unordered_set<NamespaceString> _imbalancedCollectionsCache;

    // Object under test
    std::unique_ptr<BalancerChunkSelectionPolicy> _chunkSelectionPolicy;
};

TEST_F(BalancerChunkSelectionTest, ZoneRangesOverlap) {
    setupShards({kShard0, kShard1});

    // Set up a database and a sharded collection in the metadata.
    const auto collUUID = UUID::gen();
    ChunkVersion version({OID::gen(), Timestamp(42)}, {2, 0});
    setupDatabase(kDbName, kShardId0);
    setUpCollection(kNamespace, collUUID, version);

    // Set up one chunk for the collection in the metadata.
    ChunkType chunk =
        setUpChunk(collUUID, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    auto assertRangeOverlapConflictWhenMoveChunk =
        [this, &chunk](const StringMap<ChunkRange>& zoneChunkRanges) {
            // Set up two zones whose ranges overlap.
            setUpZones(kNamespace, zoneChunkRanges);

            auto future = launchAsync([this, &chunk] {
                ThreadClient tc(getServiceContext());
                auto opCtx = Client::getCurrent()->makeOperationContext();

                auto migrateInfoStatus = _chunkSelectionPolicy.get()->selectSpecificChunkToMove(
                    opCtx.get(), kNamespace, chunk);
                ASSERT_EQUALS(ErrorCodes::RangeOverlapConflict,
                              migrateInfoStatus.getStatus().code());
            });

            expectGetStatsCommands(2);
            future.default_timed_get();
            removeAllZones(kNamespace);
        };

    assertRangeOverlapConflictWhenMoveChunk(
        {{"A", {kKeyPattern.globalMin(), BSON(kPattern << -10)}},
         {"B", {BSON(kPattern << -15), kKeyPattern.globalMax()}}});
    assertRangeOverlapConflictWhenMoveChunk(
        {{"A", {kKeyPattern.globalMin(), BSON(kPattern << -5)}},
         {"B", {BSON(kPattern << -10), kKeyPattern.globalMax()}}});
    assertRangeOverlapConflictWhenMoveChunk(
        {{"A", {kKeyPattern.globalMin(), kKeyPattern.globalMax()}},
         {"B", {BSON(kPattern << -15), kKeyPattern.globalMax()}}});
}

TEST_F(BalancerChunkSelectionTest, ZoneRangeMaxNotAlignedWithChunkMax) {
    setupShards({appendZones(kShard0, {"A"}), appendZones(kShard1, {"A"})});

    // Set up a database and a sharded collection in the metadata.
    const auto collUUID = UUID::gen();
    ChunkVersion version({OID::gen(), Timestamp(42)}, {2, 0});
    setupDatabase(kDbName, kShardId0);
    setUpCollection(kNamespace, collUUID, version);

    // Set up the zone.
    setUpZones(kNamespace, {{"A", {kKeyPattern.globalMin(), BSON(kPattern << -10)}}});

    auto assertErrorWhenMoveChunk =
        [this, &version, &collUUID](const std::vector<ChunkRange>& chunkRanges) {
            // Give shard0 all the chunks so the cluster is imbalanced.
            for (const auto& chunkRange : chunkRanges) {
                setUpChunk(collUUID, chunkRange.getMin(), chunkRange.getMax(), kShardId0, version);
                version.incMinor();
            }

            auto future = launchAsync([this] {
                ThreadClient tc(getServiceContext());
                auto opCtx = Client::getCurrent()->makeOperationContext();

                _imbalancedCollectionsCache.clear();
                const auto& chunksToMove = selectChunksToMove(opCtx.get());

                // The balancer does not bubble up the IllegalOperation error, but it is expected
                // to postpone the balancing work for the zones with the error until the chunks
                // are split appropriately.
                ASSERT_EQUALS(0U, chunksToMove.size());
            });

            const int numShards = 2;
            expectGetStatsCommands(numShards);
            for (int i = 0; i < numShards; i++) {
                expectGetStatsForBalancingCommand();
            }
            future.default_timed_get();
            removeAllChunks(kNamespace, collUUID);
        };

    assertErrorWhenMoveChunk({{kKeyPattern.globalMin(), BSON(kPattern << -5)},
                              {BSON(kPattern << -5), kKeyPattern.globalMax()}});
    assertErrorWhenMoveChunk({{kKeyPattern.globalMin(), BSON(kPattern << -15)},
                              {BSON(kPattern << -15), kKeyPattern.globalMax()}});
}

TEST_F(BalancerChunkSelectionTest, AllImbalancedCollectionsShouldEventuallyBeSelectedForBalancing) {
    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    // Set up 7 imbalanced collections (more than `kStatsForBalancingBatchSize`)
    const int numCollections = 7;
    const int maxIterations = 1000;

    for (auto i = 0; i < numCollections; ++i) {
        setUpCollectionWithChunks(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl" + std::to_string(i)),
            generateDefaultChunkRanges({kShardId0, kShardId1}));
    }

    std::set<NamespaceString> collectionsSelected;
    _imbalancedCollectionsCache.clear();

    auto i = 0;
    for (; i < maxIterations; ++i) {

        auto future = launchAsync([this, &collectionsSelected]() {
            ThreadClient tc(getServiceContext());
            auto opCtx = Client::getCurrent()->makeOperationContext();

            const auto& chunksToMove = selectChunksToMove(opCtx.get());

            for (const auto& chunkToMove : chunksToMove) {
                collectionsSelected.insert(chunkToMove.nss);
            }
        });

        expectGetStatsCommands(2 /*numShards*/);

        // Collection size distribution for each collection:
        //      Shard0 -> 512 MB
        //      Shard1 ->   0 MB
        expectGetStatsForBalancingCommands(
            {{kShardId0, 512 * 1024 * 1024 /*Bytes*/}, {kShardId1, 0 /*Bytes*/}});

        future.default_timed_get();

        if (collectionsSelected.size() == numCollections) {
            break;
        }
    }

    LOGV2(6867000,
          "AllImbalancedCollectionsShouldEventuallyBeSelectedForBalancing test results",
          "numCollectionsSelected"_attr = collectionsSelected.size(),
          "iterations"_attr = i);

    // Check that all collections were selected for balancing at least once.
    ASSERT_EQ(numCollections, collectionsSelected.size());
}

TEST_F(BalancerChunkSelectionTest, CollectionsSelectedShouldBeCached) {
    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    // Set up 4 collections
    const int numCollections = 4;
    for (auto i = 0; i < numCollections; ++i) {
        setUpCollectionWithChunks(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl" + std::to_string(i)),
            generateDefaultChunkRanges({kShardId0, kShardId1}));
    }

    std::set<NamespaceString> collectionsSelected;
    _imbalancedCollectionsCache.clear();

    for (auto i = 0; i < 5; ++i) {

        auto future = launchAsync([this, &collectionsSelected]() {
            ThreadClient tc(getServiceContext());
            auto opCtx = Client::getCurrent()->makeOperationContext();


            const auto& chunksToMove = selectChunksToMove(opCtx.get());

            for (const auto& chunkToMove : chunksToMove) {
                collectionsSelected.insert(chunkToMove.nss);
            }
        });

        expectGetStatsCommands(2 /*numShards*/);

        // Collection size distribution for each collection:
        //      Shard0 -> 512 MB
        //      Shard1 ->   0 MB
        expectGetStatsForBalancingCommands(
            {{kShardId0, 512 * 1024 * 1024 /*Bytes*/}, {kShardId1, 0 /*Bytes*/}});

        future.default_timed_get();
    }

    // Check that all selected collections are cached
    for (const auto& coll : collectionsSelected) {
        ASSERT_TRUE(_imbalancedCollectionsCache.count(coll));
    }
    ASSERT_EQ(_imbalancedCollectionsCache.size(), collectionsSelected.size());
}

TEST_F(BalancerChunkSelectionTest, CachedCollectionsShouldBeSelected) {
    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    _imbalancedCollectionsCache.clear();
    std::vector<NamespaceString> allCollections;

    // Set up 4 collections and add all them into the imbalanced collections cache
    const int numCollections = 4;
    for (auto i = 0; i < numCollections; ++i) {
        NamespaceString nss =
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl" + std::to_string(i));
        setUpCollectionWithChunks(nss, generateDefaultChunkRanges({kShardId0, kShardId1}));

        allCollections.push_back(nss);
        _imbalancedCollectionsCache.insert(nss);
    }

    std::set<NamespaceString> collectionsSelected;

    for (auto i = 0; i < 1000; ++i) {

        auto future = launchAsync([this, &collectionsSelected]() {
            ThreadClient tc(getServiceContext());
            auto opCtx = Client::getCurrent()->makeOperationContext();

            const auto& chunksToMove = selectChunksToMove(opCtx.get());

            for (const auto& chunkToMove : chunksToMove) {
                collectionsSelected.insert(chunkToMove.nss);
            }
        });

        expectGetStatsCommands(2 /*numShards*/);

        // Collection size distribution for each collection:
        //      Shard0 -> 512 MB
        //      Shard1 ->   0 MB
        expectGetStatsForBalancingCommands(
            {{kShardId0, 512 * 1024 * 1024 /*Bytes*/}, {kShardId1, 0 /*Bytes*/}});

        future.default_timed_get();

        if (collectionsSelected.size() == allCollections.size()) {
            break;
        }
    }

    // Check that all selected collections are cached
    for (const auto& nss : allCollections) {
        ASSERT_TRUE(collectionsSelected.count(nss));
    }
    ASSERT_EQ(allCollections.size(), collectionsSelected.size());
}

TEST_F(BalancerChunkSelectionTest, MaxTimeToScheduleBalancingOperationsExceeded) {
    setupShards({kShard0, kShard1, kShard2, kShard3});
    setupDatabase(kDbName, kShardId0);

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    // Set up 5 collections to process more than 1 batch
    for (auto i = 0U; i < 5; ++i) {
        setUpCollectionWithChunks(
            NamespaceString::createNamespaceString_forTest(kDbName, "coll" + std::to_string(i)),
            generateDefaultChunkRanges({kShardId0, kShardId1, kShardId2, kShardId3}));
    }

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();

        // Forcing timeout to exceed by setting it to 0
        RAIIServerParameterControllerForTest balancerChunksSelectionTimeoutMsIsZero(
            "balancerChunksSelectionTimeoutMs", 0);

        const auto& chunksToMove = selectChunksToMove(opCtx.get());

        // We know that timeout exceeded because we only got 1 migration instead of the 2 migrations
        // expected in a normal scenario with 4 shards
        ASSERT_EQUALS(1U, chunksToMove.size());
    });

    expectGetStatsCommands(4);

    // We need to get at least 1 migration per batch since the timeout only exceeds when balancer
    // has found at least one candidate migration On the other side, we must get less than 2
    // migrations per batch since the maximum number of migrations per balancing round is 2 (with 4
    // shards)
    expectGetStatsForBalancingCommandsWithOneMigration(
        4 /*numShards*/, kShardId0 /*donor*/, kShardId1 /*recipient*/);

    future.default_timed_get();
}

TEST_F(BalancerChunkSelectionTest, MoreThanOneBatchIsProcessedIfNeeded) {
    setupShards({kShard0, kShard1, kShard2, kShard3});
    setupDatabase(kDbName, kShardId0);

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    // Set up 5 collections to process 2 batches
    for (auto i = 0; i < 5; ++i) {
        setUpCollectionWithChunks(
            NamespaceString::createNamespaceString_forTest(kDbName, "coll" + std::to_string(i)),
            generateDefaultChunkRanges({kShardId0, kShardId1, kShardId2, kShardId3}));
    }

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();

        const auto& chunksToMove = selectChunksToMove(opCtx.get());

        ASSERT_EQUALS(2U, chunksToMove.size());
    });

    expectGetStatsCommands(4);

    // We are scheduling one migration on the first batch to make sure that the second batch is
    // processed
    expectGetStatsForBalancingCommandsWithOneMigration(
        4 /*numShards*/, kShardId0 /*donor*/, kShardId1 /*recipient*/);
    expectGetStatsForBalancingCommandsWithOneMigration(
        4 /*numShards*/, kShardId2 /*donor*/, kShardId3 /*recipient*/);

    future.default_timed_get();
}

TEST_F(BalancerChunkSelectionTest, StopChunksSelectionIfThereAreNoMoreShardsAvailable) {
    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    // Set up 10 collections (more than 1 batch)
    const int numCollections = 10;
    for (auto i = 0; i < numCollections; ++i) {
        setUpCollectionWithChunks(
            NamespaceString::createNamespaceString_forTest(kDbName, "coll" + std::to_string(i)),
            generateDefaultChunkRanges({kShardId0, kShardId1}));
    }

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();

        const auto& chunksToMove = selectChunksToMove(opCtx.get());

        ASSERT_EQUALS(1U, chunksToMove.size());
    });

    expectGetStatsCommands(2 /*numShards*/);

    // Only 1 batch must be processed, so we expect only 1 call to `getStatsForBalancing` since
    // the cluster has 2 shards
    expectGetStatsForBalancingCommandsWithOneMigration(
        2 /*numShards*/, kShardId0 /*donor*/, kShardId1 /*recipient*/);

    future.default_timed_get();
}

TEST_F(BalancerChunkSelectionTest, DontSelectChunksFromCollectionsWithDefragmentationEnabled) {
    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    const auto uuid1 =
        setUpCollectionWithChunks(NamespaceString::createNamespaceString_forTest(kDbName, "coll1"),
                                  generateDefaultChunkRanges({kShardId0, kShardId1}));
    const auto uuid2 =
        setUpCollectionWithChunks(NamespaceString::createNamespaceString_forTest(kDbName, "coll2"),
                                  generateDefaultChunkRanges({kShardId0, kShardId1}));

    // Enable defragmentation on collection 1
    ASSERT_OK(updateToConfigCollection(
        operationContext(),
        NamespaceString::kConfigsvrCollectionsNamespace,
        BSON(CollectionType::kUuidFieldName << uuid1),
        BSON("$set" << BSON(CollectionType::kDefragmentCollectionFieldName << true)),
        false));

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();

        const auto& chunksToMove = selectChunksToMove(opCtx.get());

        ASSERT_EQ(1, chunksToMove.size());
        ASSERT_EQ(uuid2, chunksToMove[0].uuid);
    });

    expectGetStatsCommands(2 /*numShards*/);
    expectGetStatsForBalancingCommandsWithOneMigration(
        2 /*numShards*/, kShardId0 /*donor*/, kShardId1 /*recipient*/);
    future.default_timed_get();
}

TEST_F(BalancerChunkSelectionTest, DontSelectChunksFromCollectionsWithBalancingDisabled) {
    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    const auto uuid1 = setUpCollectionWithChunks(
        NamespaceString::createNamespaceString_forTest(kDbName, "TestColl1"),
        generateDefaultChunkRanges({kShardId0, kShardId1}));
    const auto uuid2 = setUpCollectionWithChunks(
        NamespaceString::createNamespaceString_forTest(kDbName, "TestColl2"),
        generateDefaultChunkRanges({kShardId0, kShardId1}));

    // Disable balancing on collection 1
    ASSERT_OK(updateToConfigCollection(operationContext(),
                                       CollectionType::ConfigNS,
                                       BSON(CollectionType::kUuidFieldName << uuid1),
                                       BSON("$set" << BSON("noBalance" << true)),
                                       false));

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();

        const auto& chunksToMove = selectChunksToMove(opCtx.get());

        ASSERT_EQ(1, chunksToMove.size());
        ASSERT_EQ(uuid2, chunksToMove[0].uuid);
    });

    expectGetStatsCommands(2 /*numShards*/);
    expectGetStatsForBalancingCommandsWithOneMigration(
        2 /*numShards*/, kShardId0 /*donor*/, kShardId1 /*recipient*/);
    future.default_timed_get();
}

TEST_F(BalancerChunkSelectionTest, DontGetMigrationCandidatesIfAllCollectionsAreBalanced) {
    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    // Set up 7 collections (2 batches)
    const int numCollections = 7;
    for (auto i = 0; i < numCollections; ++i) {
        setUpCollectionWithChunks(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl" + std::to_string(i)),
            generateDefaultChunkRanges({kShardId0, kShardId1}));
    }

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();

        const auto& chunksToMove = selectChunksToMove(opCtx.get());

        ASSERT(chunksToMove.empty());
    });

    expectGetStatsCommands(2 /*numShards*/);

    // Expecting 2 calls to getStatsForBalancing commands since there are 7 collections (2 batches)
    // All collection distributions are balanced:
    //      Shard0 -> 512 MB
    //      Shard1 -> 500 MB
    expectGetStatsForBalancingCommands(
        {{kShardId0, 512 * 1024 * 1024 /*Bytes*/}, {kShardId1, 500 * 1024 * 1024 /*Bytes*/}});
    expectGetStatsForBalancingCommands(
        {{kShardId0, 512 * 1024 * 1024 /*Bytes*/}, {kShardId1, 500 * 1024 * 1024 /*Bytes*/}});

    future.default_timed_get();
}

TEST_F(BalancerChunkSelectionTest, SelectChunksToSplit) {
    setupShards({appendZones(kShard0, {"A"}), appendZones(kShard1, {"B"})});
    setupDatabase(kDbName, kShardId0);

    // Create 3 collections with no zones
    for (auto i = 0; i < 3; ++i) {
        setUpCollectionWithChunks(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl" + std::to_string(i)),
            generateDefaultChunkRanges({kShardId0, kShardId1}));
    }

    // Setup the collection under test with the following routing table:
    //     Shard0 -> [MinKey, 0)
    //     Shard1 -> [0, MaxKey)
    const auto nss = NamespaceString::createNamespaceString_forTest(kDbName, "TestColl");
    setUpCollectionWithChunks(nss, generateDefaultChunkRanges({kShardId0, kShardId1}));

    // Lambda function to assign specific zones to the collection under test and verify the output
    // of `selectChunksToSplit`
    auto assertSplitPoints =
        [&](const StringMap<ChunkRange>& zoneChunkRanges,
            const BSONObjIndexedMap<SplitPoints>& expectedSplitPointsPerChunk) {
            setUpZones(nss, zoneChunkRanges);

            LOGV2(7381300, "Getting split points", "zoneChunkRanges"_attr = zoneChunkRanges);

            auto future = launchAsync([&] {
                ThreadClient tc(getServiceContext());
                auto opCtx = Client::getCurrent()->makeOperationContext();

                const auto& swSplitInfo =
                    _chunkSelectionPolicy.get()->selectChunksToSplit(opCtx.get());
                ASSERT_OK(swSplitInfo.getStatus());

                for (const auto& [chunkMin, splitPoints] : expectedSplitPointsPerChunk) {

                    bool found = false;
                    for (const auto& splitInfo : swSplitInfo.getValue()) {
                        if (splitInfo.minKey.woCompare(chunkMin) == 0 && splitInfo.nss == nss) {
                            found = true;
                            ASSERT_EQ(splitPoints.size(), splitInfo.splitKeys.size());
                            for (size_t i = 0; i < splitPoints.size(); ++i) {
                                ASSERT_EQ(splitPoints[i].woCompare(splitInfo.splitKeys[i]), 0)
                                    << "Expected " << splitPoints[i].toString() << " but got "
                                    << splitInfo.splitKeys[i].toString();
                            }
                        }
                    }
                    ASSERT(found) << "Expected to find split points for chunk "
                                  << chunkMin.toString() << " but didn't";
                }

                ASSERT_EQ(expectedSplitPointsPerChunk.size(), swSplitInfo.getValue().size())
                    << "Got unexpected split points";
            });
            expectGetStatsCommands(2 /*numShards*/);
            future.default_timed_get();
            removeAllZones(nss);
        };

    // Zone A: [-20, -10)
    // Expected split point: -20, -10
    assertSplitPoints(
        {{"A", {BSON(kPattern << -20), BSON(kPattern << -10)}}},
        SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<SplitPoints>(
            {{kKeyPattern.globalMin(), {BSON(kPattern << -20), BSON(kPattern << -10)}}}));

    // Zone A: [10, 20)
    // Expected split points: 10, 20
    assertSplitPoints({{"A", {BSON(kPattern << 10), BSON(kPattern << 20)}}},
                      SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<SplitPoints>(
                          {{BSON(kPattern << 0), {BSON(kPattern << 10), BSON(kPattern << 20)}}}));

    // Zone A: [MinKey, 10)
    // Expected split point: 10
    assertSplitPoints({{"A", {kKeyPattern.globalMin(), BSON(kPattern << 10)}}},
                      SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<SplitPoints>(
                          {{BSON(kPattern << 0), {BSON(kPattern << 10)}}}));

    // Zone B: [-10, MaxKey)
    // Expected split point: -10
    assertSplitPoints({{"B", {BSON(kPattern << -10), kKeyPattern.globalMax()}}},
                      SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<SplitPoints>(
                          {{kKeyPattern.globalMin(), {BSON(kPattern << -10)}}}));


    // Zone A: [-10, 20)
    // Expected split points: 6
    assertSplitPoints({{"A", {BSON(kPattern << -10), BSON(kPattern << 20)}}},
                      SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<SplitPoints>(
                          {{kKeyPattern.globalMin(), {BSON(kPattern << -10)}},
                           {BSON(kPattern << 0), {BSON(kPattern << 20)}}}));

    // Zone B: [-20, -10)
    // Zone A: [-10, 20)
    // Expected split points: -20, -10, 20
    assertSplitPoints(
        {{"A", {BSON(kPattern << -20), BSON(kPattern << -10)}},
         {"B", {BSON(kPattern << -10), BSON(kPattern << 20)}}},
        SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<SplitPoints>(
            {{kKeyPattern.globalMin(), {BSON(kPattern << -20), BSON(kPattern << -10)}},
             {BSON(kPattern << 0), {BSON(kPattern << 20)}}}));

    // Zone A: [0, MaxKey)
    // Expected split point: NONE
    assertSplitPoints({{"A", {BSON(kPattern << 0), kKeyPattern.globalMax()}}},
                      SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<SplitPoints>());
}

}  // namespace
}  // namespace mongo
