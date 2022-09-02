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
#include "mongo/platform/random.h"
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
const uint64_t kNoMaxSize = 0;

/**
 * Constructs a shard statistics vector and a consistent mapping of chunks to shards given the
 * specified input parameters. The generated chunks have an ever increasing min value. I.e, they
 * will be in the form:
 *
 * [MinKey, 1), [1, 2), [2, 3) ... [N - 1, MaxKey)
 */
std::pair<ShardStatisticsVector, ShardToChunksMap> generateCluster(
    const vector<std::pair<ShardStatistics, size_t>>& shardsAndNumChunks) {
    int64_t totalNumChunks = 0;
    for (const auto& entry : shardsAndNumChunks) {
        totalNumChunks += std::get<1>(entry);
    }

    ShardToChunksMap chunkMap;
    ShardStatisticsVector shardStats;

    int64_t currentChunk = 0;

    ChunkVersion chunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});
    const UUID uuid = UUID::gen();

    const KeyPattern shardKeyPattern(BSON("x" << 1));

    for (auto it = shardsAndNumChunks.begin(); it != shardsAndNumChunks.end(); it++) {
        ShardStatistics shard = std::move(it->first);
        const size_t numChunks = it->second;

        // Ensure that an entry is created
        chunkMap[shard.shardId];

        for (size_t i = 0; i < numChunks; i++, currentChunk++) {
            ChunkType chunk;

            chunk.setCollectionUUID(uuid);
            chunk.setMin(currentChunk == 0 ? shardKeyPattern.globalMin()
                                           : BSON("x" << currentChunk));
            chunk.setMax(currentChunk == totalNumChunks - 1 ? shardKeyPattern.globalMax()
                                                            : BSON("x" << currentChunk + 1));
            chunk.setShard(shard.shardId);
            chunk.setVersion(chunkVersion);

            chunkVersion.incMajor();

            chunkMap[shard.shardId].push_back(std::move(chunk));
        }

        shardStats.push_back(std::move(shard));
    }

    return std::make_pair(std::move(shardStats), std::move(chunkMap));
}

MigrateInfosWithReason balanceChunks(const ShardStatisticsVector& shardStats,
                                     const DistributionStatus& distribution,
                                     bool shouldAggressivelyBalance,
                                     bool forceJumbo) {
    stdx::unordered_set<ShardId> usedShards;
    return BalancerPolicy::balance(
        shardStats, distribution, boost::none /* collDataSizeInfo */, &usedShards, forceJumbo);
}

TEST(BalancerPolicy, Basic) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 4, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0},
         {ShardStatistics(kShardId2, kNoMaxSize, 3, false, emptyZoneSet, emptyShardVersion), 3}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, SmallClusterShouldBePerfectlyBalanced) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 1, false, emptyZoneSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, emptyZoneSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId1, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, SingleChunkShouldNotMove) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 1, false, emptyZoneSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0}});
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
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyZoneSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, emptyZoneSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId2, kNoMaxSize, 1, false, emptyZoneSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId3, kNoMaxSize, 1, false, emptyZoneSet, emptyShardVersion), 1}});

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
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 4, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0},
         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(kShardId1, migrations[1].from);
    ASSERT_EQ(kShardId3, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[1].maxKey);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotPutChunksOnShardsAboveTheOptimal) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 100, false, emptyZoneSet, emptyShardVersion), 100},
         {ShardStatistics(kShardId1, kNoMaxSize, 90, false, emptyZoneSet, emptyShardVersion), 90},
         {ShardStatistics(kShardId2, kNoMaxSize, 90, false, emptyZoneSet, emptyShardVersion), 90},
         {ShardStatistics(kShardId3, kNoMaxSize, 80, false, emptyZoneSet, emptyShardVersion), 80},
         {ShardStatistics(kShardId4, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0},
         {ShardStatistics(kShardId5, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId4, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(kShardId1, migrations[1].from);
    ASSERT_EQ(kShardId5, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[1].maxKey);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotMoveChunksFromShardsBelowOptimal) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 100, false, emptyZoneSet, emptyShardVersion), 100},
         {ShardStatistics(kShardId1, kNoMaxSize, 30, false, emptyZoneSet, emptyShardVersion), 30},
         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyZoneSet, emptyShardVersion), 5},
         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId3, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseSourceShardsWithMoveNecessary) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 8, false, emptyZoneSet, emptyShardVersion), 8},
         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0},
         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0}});

    // Here kShardId0 would have been selected as a donor
    stdx::unordered_set<ShardId> usedShards{kShardId0};
    const auto [migrations, reason] =
        BalancerPolicy::balance(cluster.first,
                                DistributionStatus(kNamespace, cluster.second),
                                boost::none /* collDataSizeInfo */,
                                &usedShards,
                                false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId1, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseSourceShardsWithMoveNotNecessary) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 12, false, emptyZoneSet, emptyShardVersion), 12},
         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0},
         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0}});

    // Here kShardId0 would have been selected as a donor
    stdx::unordered_set<ShardId> usedShards{kShardId0};
    const auto [migrations, reason] =
        BalancerPolicy::balance(cluster.first,
                                DistributionStatus(kNamespace, cluster.second),
                                boost::none /* collDataSizeInfo */,
                                &usedShards,
                                false);
    ASSERT_EQ(0U, migrations.size());
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseDestinationShards) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 4, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0},
         {ShardStatistics(kShardId3, kNoMaxSize, 1, false, emptyZoneSet, emptyShardVersion), 1}});

    // Here kShardId2 would have been selected as a recipient
    stdx::unordered_set<ShardId> usedShards{kShardId2};
    const auto [migrations, reason] =
        BalancerPolicy::balance(cluster.first,
                                DistributionStatus(kNamespace, cluster.second),
                                boost::none /* collDataSizeInfo */,
                                &usedShards,
                                false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId3, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMoved) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0}});

    cluster.second[kShardId0][0].setJumbo(true);
    cluster.second[kShardId0][1].setJumbo(false);  // Only chunk 1 is not jumbo
    cluster.second[kShardId0][2].setJumbo(true);
    cluster.second[kShardId0][3].setJumbo(true);

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][1].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][1].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMovedParallel) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0},
         {ShardStatistics(kShardId2, kNoMaxSize, 2, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 0}});

    cluster.second[kShardId0][0].setJumbo(true);
    cluster.second[kShardId0][1].setJumbo(false);  // Only chunk 1 is not jumbo
    cluster.second[kShardId0][2].setJumbo(true);
    cluster.second[kShardId0][3].setJumbo(true);

    cluster.second[kShardId2][0].setJumbo(true);
    cluster.second[kShardId2][1].setJumbo(true);
    cluster.second[kShardId2][2].setJumbo(false);  // Only chunk 1 is not jumbo
    cluster.second[kShardId2][3].setJumbo(true);

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][1].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][1].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(kShardId2, migrations[1].from);
    ASSERT_EQ(kShardId3, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][2].getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][2].getMax(), *migrations[1].maxKey);
}

TEST(BalancerPolicy, DrainingSingleChunk) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyZoneSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 5}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingSingleChunkPerShard) {
    // shard0 and shard2 are draining and chunks will go to shard1 and shard3 in parallel
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyZoneSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 5},
         {ShardStatistics(kShardId2, kNoMaxSize, 2, true, emptyZoneSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 5}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);

    ASSERT_EQ(kShardId2, migrations[1].from);
    ASSERT_EQ(kShardId3, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[1].maxKey);
}

TEST(BalancerPolicy, DrainingWithTwoChunksFirstOneSelected) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyZoneSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyZoneSet, emptyShardVersion), 5}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsFirstOneSelected) {
    // shard0 and shard1 are both draining with very little chunks in them and chunks will go to
    // shard2, even though it has a lot more chunks that the other two
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, true, emptyZoneSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, true, emptyZoneSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyZoneSet, emptyShardVersion), 16}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);

    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsWontAcceptChunks) {
    // shard0 has many chunks, but can't move them to shard1 or shard2 because they are draining
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, true, emptyZoneSet, emptyShardVersion), 0},
         {ShardStatistics(kShardId2, kNoMaxSize, 0, true, emptyZoneSet, emptyShardVersion), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, DrainingSingleAppropriateShardFoundDueToZone) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, {"NYC"}, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"LAX"}, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 1, true, {"LAX"}, emptyShardVersion), 1}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(
        cluster.second[kShardId2][0].getMin(), cluster.second[kShardId2][0].getMax(), "LAX")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingNoAppropriateShardsFoundDueToZone) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, {"NYC"}, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"LAX"}, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 1, true, {"SEA"}, emptyShardVersion), 1}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(
        cluster.second[kShardId2][0].getMin(), cluster.second[kShardId2][0].getMax(), "SEA")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, NoBalancingDueToAllNodesEitherDrainingOrMaxedOut) {
    // shard0 and shard2 are draining, shard1 is maxed out
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyZoneSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId1, 1, 1, false, emptyZoneSet, emptyShardVersion), 6},
         {ShardStatistics(kShardId2, kNoMaxSize, 1, true, emptyZoneSet, emptyShardVersion), 1}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, BalancerRespectsMaxShardSizeOnlyBalanceToNonMaxed) {
    // Note that maxSize of shard0 is 1, and it is therefore overloaded with currSize = 3. Other
    // shards have maxSize = 0 = unset. Even though the overloaded shard has the least number of
    // less chunks, we shouldn't move chunks to that shard.
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, 1, 3, false, emptyZoneSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, emptyZoneSet, emptyShardVersion), 5},
         {ShardStatistics(kShardId2, kNoMaxSize, 10, false, emptyZoneSet, emptyShardVersion), 10}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
}

TEST(BalancerPolicy, BalancerRespectsMaxShardSizeWhenAllBalanced) {
    // Note that maxSize of shard0 is 1, and it is therefore overloaded with currSize = 4. Other
    // shards have maxSize = 0 = unset. We check that being over the maxSize is NOT equivalent to
    // draining, we don't want to empty shards for no other reason than they are over this limit.
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, 1, 4, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyZoneSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 4, false, emptyZoneSet, emptyShardVersion), 4}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, DistributionStatus(kNamespace, cluster.second), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, BalancerRespectsZonesWhenDraining) {
    // shard1 drains the proper chunk to shard0, even though it is more loaded than shard2
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 6},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, true, {"a", "b"}, emptyShardVersion), 2},
         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, {"b"}, emptyShardVersion), 2}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 7), "a")));
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(BSON("x" << 8), kMaxBSONKey, "b")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId1, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, BalancerRespectsZonePolicyBeforeImbalance) {
    // There is a large imbalance between shard0 and shard1, but the balancer must first fix the
    // chunks, which are on a wrong shard due to zone policy
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 2},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 6},
         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyZoneSet, emptyShardVersion), 2}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 100), "a")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerFixesIncorrectZonesWithCrossShardViolationOfZones) {
    // The zone policy dictates that the same shard must donate and also receive chunks. The test
    // validates that the same shard is not used as a donor and recipient as part of the same round.
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 3},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 3},
         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, {"b"}, emptyShardVersion), 3}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 1), "b")));
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(BSON("x" << 8), kMaxBSONKey, "a")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerFixesIncorrectZonesInOtherwiseBalancedCluster) {
    // Chunks are balanced across shards, but there are wrong zones, which need to be fixed
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 3},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 3},
         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyZoneSet, emptyShardVersion), 3}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 10), "a")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerZoneAlreadyBalanced) {
    // Chunks are balanced across shards for the zone.
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 3, false, {"a"}, emptyShardVersion), 2},
         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"a"}, emptyShardVersion), 2}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, kMaxBSONKey, "a")));
    ASSERT(balanceChunks(cluster.first, distribution, false, false).first.empty());
}

TEST(BalancerPolicy, BalancerMostOverLoadShardHasMultipleZones) {
    // shard0 has chunks [MinKey, 1), [1, 2), [2, 3), [3, 4), [4, 5), so two chunks each
    // for zones "b" and "c". So [1, 2) is expected to be moved to shard1 in round 1.
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a", "b", "c"}, emptyShardVersion), 5},
         {ShardStatistics(kShardId1, kNoMaxSize, 1, false, {"b"}, emptyShardVersion), 1},
         {ShardStatistics(kShardId2, kNoMaxSize, 1, false, {"c"}, emptyShardVersion), 1}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 1), "a")));
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 3), "b")));
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(BSON("x" << 3), BSON("x" << 5), "c")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][1].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][1].getMax(), *migrations[0].maxKey);
}

TEST(BalancerPolicy, BalancerMostOverLoadShardHasMultipleZonesSkipZoneWithShardInUse) {
    // shard0 has chunks [MinKey, 1), [1, 2), [2, 3), [3, 4), [4, 5), so two chunks each
    // for zones "b" and "c". So [3, 4) is expected to be moved to shard2 because shard1 is
    // in use.
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a", "b", "c"}, emptyShardVersion), 5},
         {ShardStatistics(kShardId1, kNoMaxSize, 1, false, {"b"}, emptyShardVersion), 1},
         {ShardStatistics(kShardId2, kNoMaxSize, 1, false, {"c"}, emptyShardVersion), 1}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 1), "a")));
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 3), "b")));
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(BSON("x" << 3), BSON("x" << 5), "c")));

    stdx::unordered_set<ShardId> usedShards{kShardId1};
    const auto [migrations, reason] = BalancerPolicy::balance(
        cluster.first, distribution, boost::none /* collDataSizeInfo */, &usedShards, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][3].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][3].getMax(), *migrations[0].maxKey);
}

TEST(BalancerPolicy, BalancerFixesIncorrectZonesInOtherwiseBalancedClusterParallel) {
    // Chunks are balanced across shards, but there are wrong zones, which need to be fixed
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 3},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 3},
         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyZoneSet, emptyShardVersion), 3},
         {ShardStatistics(kShardId3, kNoMaxSize, 5, false, emptyZoneSet, emptyShardVersion), 3}});

    DistributionStatus distribution(kNamespace, cluster.second);
    ASSERT_OK(distribution.addRangeToZone(ZoneRange(kMinBSONKey, BSON("x" << 20), "a")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);

    ASSERT_EQ(kShardId3, migrations[1].from);
    ASSERT_EQ(kShardId1, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId3][0].getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId3][0].getMax(), *migrations[1].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerHandlesNoShardsWithZone) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, false, emptyZoneSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, emptyZoneSet, emptyShardVersion), 2}});

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
