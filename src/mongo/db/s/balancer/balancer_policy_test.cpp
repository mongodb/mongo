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

#include "mongo/db/keypattern.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using std::map;
using std::string;
using std::stringstream;
using std::vector;

using ShardStatistics = ClusterStatistics::ShardStatistics;

const auto emptyZoneSet = std::set<std::string>();
const std::string emptyShardVersion = "";
const auto kShardId0 = ShardId("shard0");
const auto kShardId1 = ShardId("shard1");
const auto kShardId2 = ShardId("shard2");
const auto kShardId3 = ShardId("shard3");
const auto kShardId4 = ShardId("shard4");
const auto kShardId5 = ShardId("shard5");
const NamespaceString kNamespace("TestDB", "TestColl");

/**
 * Constructs a shard statistics vector and a consistent mapping of chunks to shards given the
 * specified input parameters. The generated chunks have an ever increasing min value. I.e, they
 * will be in the form:
 *
 * [MinKey, 1), [1, 2), [2, 3) ... [N - 1, MaxKey)
 */
std::pair<ShardStatisticsVector, ShardToChunksMap> generateCluster(
    const vector<ShardStatistics>& statsVector) {

    // Distribute one chunk per shard, no matter the owned data size.
    int64_t totalNumChunks = statsVector.size();

    ShardToChunksMap chunkMap;
    ShardStatisticsVector shardStats;

    int64_t currentChunk = 0;

    ChunkVersion chunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});
    const UUID uuid = UUID::gen();

    const KeyPattern shardKeyPattern(BSON("x" << 1));

    for (const auto& shard : statsVector) {
        // Ensure that an entry is created
        chunkMap[shard.shardId];

        ChunkType chunk;

        chunk.setCollectionUUID(uuid);
        chunk.setMin(currentChunk == 0 ? shardKeyPattern.globalMin() : BSON("x" << currentChunk));
        chunk.setMax(currentChunk == totalNumChunks - 1 ? shardKeyPattern.globalMax()
                                                        : BSON("x" << ++currentChunk));
        chunk.setShard(shard.shardId);
        chunk.setVersion(chunkVersion);

        chunkVersion.incMajor();
        chunkMap[shard.shardId].push_back(std::move(chunk));

        shardStats.push_back(std::move(shard));
    }

    return std::make_pair(std::move(shardStats), std::move(chunkMap));
}

stdx::unordered_set<ShardId> getAllShardIds(const ShardStatisticsVector& shardStats) {
    stdx::unordered_set<ShardId> shards;
    std::transform(shardStats.begin(),
                   shardStats.end(),
                   std::inserter(shards, shards.end()),
                   [](const ShardStatistics& shardStatistics) { return shardStatistics.shardId; });
    return shards;
}

CollectionDataSizeInfoForBalancing buildDataSizeInfoForBalancingFromShardStats(
    const ShardStatisticsVector& shardStats) {
    std::map<ShardId, int64_t> collSizePerShard;
    for (const auto& shard : shardStats) {
        collSizePerShard.try_emplace(shard.shardId, shard.currSizeBytes);
    }

    return CollectionDataSizeInfoForBalancing(std::move(collSizePerShard),
                                              ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes);
}

MigrateInfosWithReason balanceChunks(const ShardStatisticsVector& shardStats,
                                     const DistributionStatus& distribution,
                                     bool shouldAggressivelyBalance,
                                     bool forceJumbo) {
    auto availableShards = getAllShardIds(shardStats);

    return BalancerPolicy::balance(shardStats,
                                   distribution,
                                   buildDataSizeInfoForBalancingFromShardStats(shardStats),
                                   &availableShards,
                                   forceJumbo);
}

TEST(BalancerPolicy, Basic) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         4 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1, 0, false, emptyZoneSet, emptyShardVersion),
                         ShardStatistics(kShardId2,
                                         3 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t())});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, SmallSingleChunkShouldNotMove) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1, 0, false, emptyZoneSet, emptyShardVersion)});

    {
        auto [migrations, reason] = balanceChunks(
            cluster.first, DistributionStatus(kNamespace, cluster.second), true, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
    {
        auto [migrations, reason] = balanceChunks(
            cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
}

TEST(BalancerPolicy, BalanceThresholdObeyed) {
    auto cluster = generateCluster({
        ShardStatistics(kShardId0,
                        2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                        false,
                        emptyZoneSet,
                        emptyShardVersion,
                        ShardStatistics::use_bytes_t()),
        ShardStatistics(kShardId1,
                        2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                        false,
                        emptyZoneSet,
                        emptyShardVersion,
                        ShardStatistics::use_bytes_t()),

        ShardStatistics(kShardId2,
                        ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                        false,
                        emptyZoneSet,
                        emptyShardVersion,
                        ShardStatistics::use_bytes_t()),
        ShardStatistics(kShardId3,
                        ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                        false,
                        emptyZoneSet,
                        emptyShardVersion,
                        ShardStatistics::use_bytes_t()),
    });

    {
        auto [migrations, reason] = balanceChunks(
            cluster.first, DistributionStatus(kNamespace, cluster.second), true, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
    {
        auto [migrations, reason] = balanceChunks(
            cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
}

TEST(BalancerPolicy, ParallelBalancing) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         4 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         4 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2, 0, false, emptyZoneSet, emptyShardVersion),
                         ShardStatistics(kShardId3, 0, false, emptyZoneSet, emptyShardVersion)});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(kShardId1, migrations[1].from);
    ASSERT_EQ(kShardId3, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotScheduleMigrationsOnShardsAboveTheThreshold) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         100 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         90 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2,
                                         90 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId3,
                                         89 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId4, 0, false, emptyZoneSet, emptyShardVersion),
                         ShardStatistics(kShardId5, 0, false, emptyZoneSet, emptyShardVersion)});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId4, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(kShardId1, migrations[1].from);
    ASSERT_EQ(kShardId5, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseSourceShardsWithMoveNecessary) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         8 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         4 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2, 0, false, emptyZoneSet, emptyShardVersion),
                         ShardStatistics(kShardId3, 0, false, emptyZoneSet, emptyShardVersion)});

    const auto shardStats = cluster.first;

    // Here kShardId0 would have been selected as a donor
    auto availableShards = getAllShardIds(shardStats);
    availableShards.erase(kShardId0);
    const auto [migrations, reason] =
        BalancerPolicy::balance(shardStats,
                                DistributionStatus(kNamespace, cluster.second),
                                buildDataSizeInfoForBalancingFromShardStats(shardStats),
                                &availableShards,
                                false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId1, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseDestinationShards) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         4 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         4 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2, 0, false, emptyZoneSet, emptyShardVersion),
                         ShardStatistics(kShardId3,
                                         1 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t())});

    const auto shardStats = cluster.first;

    // Here kShardId2 would have been selected as a recipient
    auto availableShards = getAllShardIds(shardStats);
    availableShards.erase(kShardId2);
    const auto [migrations, reason] =
        BalancerPolicy::balance(shardStats,
                                DistributionStatus(kNamespace, cluster.second),
                                buildDataSizeInfoForBalancingFromShardStats(shardStats),
                                &availableShards,
                                false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId3, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotMoveDataFromShardsBelowIdealZoneSize) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         100 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         30 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2,
                                         5 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId3, 0, false, emptyZoneSet, emptyShardVersion)});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId3, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMoved) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         4 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1, 0, false, emptyZoneSet, emptyShardVersion)});

    cluster.second[kShardId0][0].setJumbo(true);

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, JumboChunksNotMovedParallel) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         4 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1, 0, false, emptyZoneSet, emptyShardVersion),
                         ShardStatistics(kShardId2,
                                         4 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId3, 0, false, emptyZoneSet, emptyShardVersion)});

    cluster.second[kShardId0][0].setJumbo(true);

    cluster.second[kShardId2][0].setJumbo(true);

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, DrainingFromShardWithFewData) {
    // shard1 is draining and chunks will go to shard0, even though it has a lot more data
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         20 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false /* draining */,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true /* draining */,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t())});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId1, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingSingleChunkPerShard) {
    // shard0 and shard2 are draining and chunks will go to shard1 and shard3 in parallel
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1, 0, false, emptyZoneSet, emptyShardVersion),
                         ShardStatistics(kShardId2,
                                         2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId3, 0, false, emptyZoneSet, emptyShardVersion)});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);

    ASSERT_EQ(kShardId2, migrations[1].from);
    ASSERT_EQ(kShardId3, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, DrainingMultipleShardsFirstOneSelected) {
    // shard0 and shard1 are both draining with very little data in them and chunks will go to
    // shard2, even though it has a lot more data that the other two
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         50 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false /* draining */,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         5 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true /* draining */,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2,
                                         5 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true /* draining */,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t())});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);

    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsWontAcceptMigrations) {
    // shard0 has many data, but can't move them to shard1 or shard2 because they are draining
    auto cluster = generateCluster(
        {ShardStatistics(kShardId0,
                         20 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                         false /* draining */,
                         emptyZoneSet,
                         emptyShardVersion,
                         ShardStatistics::use_bytes_t()),
         ShardStatistics(kShardId1, 0, true /* draining */, emptyZoneSet, emptyShardVersion),
         ShardStatistics(kShardId2, 0, true /* draining */, emptyZoneSet, emptyShardVersion)});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].to);
}

TEST(BalancerPolicy, DrainingSingleAppropriateShardFoundDueToZone) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         {"NYC"},
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         {"LAX"},
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2,
                                         ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true,
                                         {"LAX"},
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t())});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(
        cluster.second[kShardId2][0].getMin(), cluster.second[kShardId2][0].getMax(), "LAX")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingNoAppropriateShardsFoundDueToZone) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         {"NYC"},
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         {"LAX"},
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2,
                                         ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true,
                                         {"SEA"},
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t())});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(
        cluster.second[kShardId2][0].getMin(), cluster.second[kShardId2][0].getMax(), "SEA")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, NoBalancingDueToAllNodesDraining) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2,
                                         ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t())});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, BalancerRespectsZonesWhenDraining) {
    // shard1 drains the proper chunk to shard0, even though it is more loaded than shard2
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         5 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         {"a"},
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         5 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         true,
                                         {"a", "b"},
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId2,
                                         5 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         {"b"},
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t())});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 7), "a")));
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(BSON("x" << 8), kMaxBSONKey, "b")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId1, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, BalancerZoneAlreadyBalanced) {
    // Chunks are balanced across shards for the zone.
    auto cluster = generateCluster({
        ShardStatistics(kShardId0,
                        3 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                        false,
                        {"a"},
                        emptyShardVersion,
                        ShardStatistics::use_bytes_t()),
        ShardStatistics(kShardId1,
                        2 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                        false,
                        {"a"},
                        emptyShardVersion,
                        ShardStatistics::use_bytes_t()),
    });

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, kMaxBSONKey, "a")));
    ASSERT(balanceChunks(cluster.first, distribution, false, false).first.empty());
}

TEST(BalancerPolicy, BalancerHandlesNoShardsWithZone) {
    auto cluster =
        generateCluster({ShardStatistics(kShardId0,
                                         5 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t()),
                         ShardStatistics(kShardId1,
                                         5 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes,
                                         false,
                                         emptyZoneSet,
                                         emptyShardVersion,
                                         ShardStatistics::use_bytes_t())});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(
        distribution.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 7), "NonExistentZone")));

    ASSERT(balanceChunks(cluster.first, distribution, false, false).first.empty());
}

TEST(DistributionStatus, AddZoneRangeOverlap) {
    DistributionStatus d(kNamespace, ShardToChunksMap{});

    // Note that there is gap between 10 and 20 for which there is no zone
    ASSERT_OK(d.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "a")));
    ASSERT_OK(d.addRangeToZone(ZoneRange(BSON("x" << 20), BSON("x" << 30), "b")));

    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              d.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 2), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              d.addRangeToZone(ZoneRange(BSON("x" << -1), BSON("x" << 5), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              d.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 9), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              d.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              d.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 25), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              d.addRangeToZone(ZoneRange(BSON("x" << -1), BSON("x" << 32), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              d.addRangeToZone(ZoneRange(BSON("x" << 25), kMaxBSONKey, "d")));
}

TEST(DistributionStatus, ChunkZonesSelectorWithRegularKeys) {
    DistributionStatus d(kNamespace, ShardToChunksMap{});

    ASSERT_OK(d.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "a")));
    ASSERT_OK(d.addRangeToZone(ZoneRange(BSON("x" << 10), BSON("x" << 20), "b")));
    ASSERT_OK(d.addRangeToZone(ZoneRange(BSON("x" << 20), BSON("x" << 30), "c")));

    {
        ChunkType chunk;
        chunk.setMin(kMinBSONKey);
        chunk.setMax(BSON("x" << 1));
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 0));
        chunk.setMax(BSON("x" << 1));
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 1));
        chunk.setMax(BSON("x" << 5));
        ASSERT_EQUALS("a", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 10));
        chunk.setMax(BSON("x" << 20));
        ASSERT_EQUALS("b", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 15));
        chunk.setMax(BSON("x" << 20));
        ASSERT_EQUALS("b", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 25));
        chunk.setMax(BSON("x" << 30));
        ASSERT_EQUALS("c", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 35));
        chunk.setMax(BSON("x" << 40));
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 30));
        chunk.setMax(kMaxBSONKey);
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 40));
        chunk.setMax(kMaxBSONKey);
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }
}

TEST(DistributionStatus, ChunkZonesSelectorWithMinMaxKeys) {
    DistributionStatus d(kNamespace, ShardToChunksMap{});

    ASSERT_OK(d.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << -100), "a")));
    ASSERT_OK(d.addRangeToZone(ZoneRange(BSON("x" << -10), BSON("x" << 10), "b")));
    ASSERT_OK(d.addRangeToZone(ZoneRange(BSON("x" << 100), kMaxBSONKey, "c")));

    {
        ChunkType chunk;
        chunk.setMin(kMinBSONKey);
        chunk.setMax(BSON("x" << -100));
        ASSERT_EQUALS("a", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << -100));
        chunk.setMax(BSON("x" << -11));
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << -10));
        chunk.setMax(BSON("x" << 0));
        ASSERT_EQUALS("b", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 0));
        chunk.setMax(BSON("x" << 10));
        ASSERT_EQUALS("b", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 10));
        chunk.setMax(BSON("x" << 20));
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 10));
        chunk.setMax(BSON("x" << 100));
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 200));
        chunk.setMax(kMaxBSONKey);
        ASSERT_EQUALS("c", d.getZoneForChunk(chunk));
    }
}

}  // namespace
}  // namespace mongo
