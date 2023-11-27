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

#include <algorithm>
#include <cstdint>
#include <iosfwd>
#include <iterator>
#include <memory>
#include <type_traits>

#include <absl/container/node_hash_set.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using std::map;
using std::string;
using std::stringstream;
using std::vector;

using ShardStatistics = ClusterStatistics::ShardStatistics;

auto& kDefaultMaxChunkSizeBytes = ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes;
const auto emptyZoneSet = std::set<std::string>();
const std::string emptyShardVersion = "";
const NamespaceString kNamespace =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const KeyPattern kShardKeyPattern(BSON("x" << 1));


struct ShardSpec {
    ShardSpec(size_t numChunks,
              size_t currSizeBytes,
              bool isDraining = false,
              const std::set<std::string>& shardZones = emptyZoneSet)
        : numChunks(numChunks),
          currSizeBytes(currSizeBytes),
          isDraining(isDraining),
          shardZones(shardZones) {}

    size_t numChunks;
    size_t currSizeBytes;
    bool isDraining;
    std::set<std::string> shardZones;
};

ShardId getShardId(size_t shardIdx) {
    return {std::string(str::stream() << "shard_" << shardIdx)};
}

/**
 * Constructs a shard statistics vector and a consistent mapping of chunks to shards given the
 * specified input parameters. The generated chunks have an ever increasing min value. I.e, they
 * will be in the form:
 *
 * [MinKey, 1), [1, 2), [2, 3) ... [N - 1, MaxKey)
 */
std::pair<ShardStatisticsVector, ShardToChunksMap> generateCluster(
    const vector<ShardSpec>& shardsSpec) {

    const auto totalNumChunks = [&] {
        size_t total{0};
        for (const auto& shardSpec : shardsSpec) {
            total += shardSpec.numChunks;
        }
        return total;
    }();

    ShardToChunksMap chunkMap;
    ShardStatisticsVector shardStats;

    size_t currentChunk{0};

    ChunkVersion chunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});
    const UUID uuid = UUID::gen();

    size_t currentShardIdx{0};

    for (const auto& shardSpec : shardsSpec) {
        const auto shardId = getShardId(currentShardIdx++);

        // Ensure that an entry is created
        chunkMap[shardId];

        for (size_t i = 0; i < shardSpec.numChunks; i++, currentChunk++) {
            ChunkType chunk;

            chunk.setCollectionUUID(uuid);
            chunk.setMin(currentChunk == 0 ? kShardKeyPattern.globalMin()
                                           : BSON("x" << (long long)currentChunk));
            chunk.setMax(currentChunk == totalNumChunks - 1
                             ? kShardKeyPattern.globalMax()
                             : BSON("x" << (long long)currentChunk + 1));
            chunk.setShard(shardId);
            chunk.setVersion(chunkVersion);

            chunkVersion.incMajor();

            chunkMap[shardId].push_back(std::move(chunk));
        }

        shardStats.emplace_back(shardId,
                                shardSpec.currSizeBytes,
                                shardSpec.isDraining,
                                shardSpec.shardZones,
                                emptyShardVersion,
                                ShardStatistics::use_bytes_t());
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
                                              kDefaultMaxChunkSizeBytes);
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
    auto cluster = generateCluster({{4, 4 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes},
                                    {3, 3 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, BasicWithManyChunks) {
    auto cluster = generateCluster({{10, 1 * kDefaultMaxChunkSizeBytes},
                                    {2, 3 * kDefaultMaxChunkSizeBytes},
                                    {20, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, SmallSingleChunkShouldNotMove) {
    auto cluster =
        generateCluster({{1, 1 * kDefaultMaxChunkSizeBytes}, {0, 0 * kDefaultMaxChunkSizeBytes}});
    {
        auto [migrations, reason] = balanceChunks(
            cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), true, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
    {
        auto [migrations, reason] =
            balanceChunks(cluster.first,
                          DistributionStatus(kNamespace, cluster.second, ZoneInfo()),
                          false,
                          false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
}

TEST(BalancerPolicy, BalanceThresholdObeyed) {
    auto cluster = generateCluster({{2, 2 * kDefaultMaxChunkSizeBytes},
                                    {2, 2 * kDefaultMaxChunkSizeBytes},
                                    {1, 1 * kDefaultMaxChunkSizeBytes},
                                    {1, 1 * kDefaultMaxChunkSizeBytes}});

    {
        auto [migrations, reason] = balanceChunks(
            cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), true, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
    {
        auto [migrations, reason] =
            balanceChunks(cluster.first,
                          DistributionStatus(kNamespace, cluster.second, ZoneInfo()),
                          false,
                          false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
}

TEST(BalancerPolicy, ParallelBalancing) {
    auto cluster = generateCluster({{4, 4 * kDefaultMaxChunkSizeBytes},
                                    {4, 4 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(getShardId(1), migrations[1].from);
    ASSERT_EQ(getShardId(3), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotScheduleMigrationsOnShardsAboveTheThreshold) {
    auto cluster = generateCluster({{100, 100 * kDefaultMaxChunkSizeBytes},
                                    {90, 90 * kDefaultMaxChunkSizeBytes},
                                    {90, 90 * kDefaultMaxChunkSizeBytes},
                                    {89, 89 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(4), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(getShardId(1), migrations[1].from);
    ASSERT_EQ(getShardId(5), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotMoveChunksFromShardsBelowOptimal) {
    auto cluster = generateCluster({{100, 100 * kDefaultMaxChunkSizeBytes},
                                    {30, 30 * kDefaultMaxChunkSizeBytes},
                                    {5, 5 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(3), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseSourceShardsWithMoveNecessary) {
    auto cluster = generateCluster({{8, 8 * kDefaultMaxChunkSizeBytes},
                                    {4, 4 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes}});

    const auto& shardStats = cluster.first;

    // Here getShardId(0) would have been selected as a donor
    auto availableShards = getAllShardIds(shardStats);
    availableShards.erase(getShardId(0));
    const auto [migrations, reason] =
        BalancerPolicy::balance(shardStats,
                                DistributionStatus(kNamespace, cluster.second, ZoneInfo()),
                                buildDataSizeInfoForBalancingFromShardStats(shardStats),
                                &availableShards,
                                false);

    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseSourceShardsWithMoveNotNecessary) {
    auto cluster = generateCluster({{12, 12 * kDefaultMaxChunkSizeBytes},
                                    {4, 4 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes}});

    // Here getShardId(0) would have been selected as a donor
    auto availableShards = getAllShardIds(cluster.first);
    availableShards.erase(getShardId(0));
    const auto [migrations, reason] =
        BalancerPolicy::balance(cluster.first,
                                DistributionStatus(kNamespace, cluster.second, ZoneInfo()),
                                buildDataSizeInfoForBalancingFromShardStats(cluster.first),
                                &availableShards,
                                false);
    ASSERT_EQ(0U, migrations.size());
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseDestinationShards) {
    auto cluster = generateCluster({{4, 4 * kDefaultMaxChunkSizeBytes},
                                    {4, 4 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes},
                                    {1, 1 * kDefaultMaxChunkSizeBytes}});

    const auto& shardStats = cluster.first;

    // Here getShardId(2) would have been selected as a recipient
    auto availableShards = getAllShardIds(shardStats);
    availableShards.erase(getShardId(2));
    const auto [migrations, reason] =
        BalancerPolicy::balance(shardStats,
                                DistributionStatus(kNamespace, cluster.second, ZoneInfo()),
                                buildDataSizeInfoForBalancingFromShardStats(shardStats),
                                &availableShards,
                                false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(3), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotMoveDataFromShardsBelowIdealZoneSize) {
    auto cluster = generateCluster({{1, 100 * kDefaultMaxChunkSizeBytes},
                                    {1, 30 * kDefaultMaxChunkSizeBytes},
                                    {1, 5 * kDefaultMaxChunkSizeBytes},
                                    {1, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(3), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMoved) {
    auto cluster =
        generateCluster({{4, 4 * kDefaultMaxChunkSizeBytes}, {0, 0 * kDefaultMaxChunkSizeBytes}});

    cluster.second[getShardId(0)][0].setJumbo(true);
    cluster.second[getShardId(0)][1].setJumbo(false);  // Only chunk 1 is not jumbo
    cluster.second[getShardId(0)][2].setJumbo(true);
    cluster.second[getShardId(0)][3].setJumbo(true);

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][1].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMovedParallel) {
    auto cluster = generateCluster({{4, 4 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes},
                                    {4, 4 * kDefaultMaxChunkSizeBytes},
                                    {0, 0 * kDefaultMaxChunkSizeBytes}});

    cluster.second[getShardId(0)][0].setJumbo(true);
    cluster.second[getShardId(0)][1].setJumbo(false);  // Only chunk 1 is not jumbo
    cluster.second[getShardId(0)][2].setJumbo(true);
    cluster.second[getShardId(0)][3].setJumbo(true);

    cluster.second[getShardId(2)][0].setJumbo(true);
    cluster.second[getShardId(2)][1].setJumbo(true);
    cluster.second[getShardId(2)][2].setJumbo(false);  // Only chunk 1 is not jumbo
    cluster.second[getShardId(2)][3].setJumbo(true);

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][1].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(getShardId(2), migrations[1].from);
    ASSERT_EQ(getShardId(3), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][2].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, DrainingFromShardWithFewData) {
    // shard1 is draining and chunks will go to shard0, even though it has a lot more data
    auto cluster = generateCluster({{1, 20 * kDefaultMaxChunkSizeBytes, false /* isDraining */},
                                    {1, 1 * kDefaultMaxChunkSizeBytes, true /* isDrainig */}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingSingleChunk) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto cluster = generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, true /* isDraining */},
                                    {5, 0 * kDefaultMaxChunkSizeBytes, false /* isDraining */}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingSingleChunkPerShard) {
    // shard0 and shard2 are draining and chunks will go to shard1 and shard3 in parallel
    auto cluster = generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, true},
                                    {5, 0 * kDefaultMaxChunkSizeBytes, false},
                                    {1, 2 * kDefaultMaxChunkSizeBytes, true},
                                    {5, 0 * kDefaultMaxChunkSizeBytes, false}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);

    ASSERT_EQ(getShardId(2), migrations[1].from);
    ASSERT_EQ(getShardId(3), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, DrainingMultipleShardsAtLeastOneSelected) {
    // shard1 and shard2 are both draining with very little data in them and chunks will go to
    // shard0, even though it has a lot more data that the other two
    auto cluster = generateCluster({{1, 50 * kDefaultMaxChunkSizeBytes, false},
                                    {1, 5 * kDefaultMaxChunkSizeBytes, true},
                                    {1, 5 * kDefaultMaxChunkSizeBytes, true}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsFirstOneSelected) {
    // shard0 and shard1 are both draining with very little data in them and chunks will go to
    // shard2, even though it has a lot more chunks that the other two
    auto cluster = generateCluster({{1, 5 * kDefaultMaxChunkSizeBytes, true},
                                    {2, 5 * kDefaultMaxChunkSizeBytes, true},
                                    {16, 5 * kDefaultMaxChunkSizeBytes, false}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsWontAcceptMigrations) {
    // shard0 has many data, but can't move them to shard1 or shard2 because they are draining
    auto cluster = generateCluster({{4, 2 * kDefaultMaxChunkSizeBytes, false},
                                    {0, 0 * kDefaultMaxChunkSizeBytes, true},
                                    {0, 0 * kDefaultMaxChunkSizeBytes, true}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, DrainingWithTwoChunksFirstOneSelected) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto cluster = generateCluster(
        {{2, 2 * kDefaultMaxChunkSizeBytes, true}, {5, 0 * kDefaultMaxChunkSizeBytes, false}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);

    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingSingleAppropriateShardFoundDueToZone) {
    auto cluster = generateCluster({{4, 2 * kDefaultMaxChunkSizeBytes, false, {"NYC"}},
                                    {4, 2 * kDefaultMaxChunkSizeBytes, false, {"LAX"}},
                                    {1, 1 * kDefaultMaxChunkSizeBytes, true, {"LAX"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(2)][0].getMin(),
                                                cluster.second[getShardId(2)][0].getMax(),
                                                "LAX")));
    DistributionStatus distribution(kNamespace, cluster.second, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingNoAppropriateShardsFoundDueToZone) {
    auto cluster = generateCluster({{4, 2 * kDefaultMaxChunkSizeBytes, false, {"NYC"}},
                                    {4, 2 * kDefaultMaxChunkSizeBytes, false, {"LAX"}},
                                    {1, 1 * kDefaultMaxChunkSizeBytes, true, {"SEA"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(2)][0].getMin(),
                                                cluster.second[getShardId(2)][0].getMax(),
                                                "SEA")));
    DistributionStatus distribution(kNamespace, cluster.second, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, NoBalancingDueToAllNodesDraining) {
    auto cluster = generateCluster({{1, 5 * kDefaultMaxChunkSizeBytes, true, emptyZoneSet},
                                    {1, 1 * kDefaultMaxChunkSizeBytes, true, emptyZoneSet}});

    const auto [migrations, reason] = balanceChunks(
        cluster.first, DistributionStatus(kNamespace, cluster.second, ZoneInfo()), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, BalancerRespectsZonesWhenDraining) {
    // shard1 drains the proper chunk to shard0, even though it is more loaded than shard2
    auto cluster = generateCluster({{6, 6 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {2, 2 * kDefaultMaxChunkSizeBytes, true, {"a", "b"}},
                                    {2, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 7), "a")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 8), kShardKeyPattern.globalMax(), "b")));

    DistributionStatus distribution(kNamespace, cluster.second, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, BalancerRespectsZonePolicyBeforeImbalance) {
    // There is a large imbalance between shard0 and shard1, but the balancer must first fix the
    // chunks, which are on a wrong shard due to zone policy
    auto cluster = generateCluster({{2, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {6, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {5, 2 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet}});

    DistributionStatus distribution(kNamespace, cluster.second, ZoneInfo());
    ASSERT_OK(distribution.zoneInfo().addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 100), "a")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerFixesIncorrectZonesWithCrossShardViolationOfZones) {
    // The zone policy dictates that the same shard must donate and also receive chunks. The
    // test validates that the same shard is not used as a donor and recipient as part of the
    // same round.
    auto cluster = generateCluster({{3, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {3, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {3, 5 * kDefaultMaxChunkSizeBytes, false, {"b"}}});

    DistributionStatus distribution(kNamespace, cluster.second, ZoneInfo());
    ASSERT_OK(distribution.zoneInfo().addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 1), "b")));
    ASSERT_OK(distribution.zoneInfo().addRangeToZone(
        ZoneRange(BSON("x" << 8), kShardKeyPattern.globalMax(), "a")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerFixesIncorrectZonesInOtherwiseBalancedCluster) {
    // Chunks are balanced across shards, but there are wrong zones, which need to be fixed
    auto cluster = generateCluster({{3, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {3, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {3, 5 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet}});

    DistributionStatus distribution(kNamespace, cluster.second, ZoneInfo());
    ASSERT_OK(distribution.zoneInfo().addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 10), "a")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerZoneAlreadyBalanced) {
    // Chunks are balanced across shards for the zone.
    auto cluster = generateCluster({{3, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {2, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));
    DistributionStatus distribution(kNamespace, cluster.second, std::move(zoneInfo));
    ASSERT(balanceChunks(cluster.first, distribution, false, false).first.empty());
}

TEST(BalancerPolicy, ScheduleMigrationForChunkViolatingZone) {
    // Zone violation: shard1 owns a chunk from zone "a"
    auto cluster = generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {1, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));
    DistributionStatus distribution(kNamespace, cluster.second, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, ScheduleParallelMigrationsForZoneViolations) {
    // shard2 and shard3 own chunks from zone "a" that are violating the shards zone
    auto cluster = generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {1, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {1, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}},
                                    {1, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));

    DistributionStatus distribution(kNamespace, cluster.second, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(2U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);

    ASSERT_EQ(getShardId(3), migrations[1].from);
    ASSERT_EQ(getShardId(1), migrations[1].to);

    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, DrainingHasPrecedenceOverZoneViolation) {
    // shard1 owns a chunk from zone "a" that is violating the shards zone, however shard2 is in
    // draining mode so it has preference over shard1
    auto cluster = generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {1, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}},
                                    {1, 2 * kDefaultMaxChunkSizeBytes, true /*draining*/, {"a"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));
    DistributionStatus distribution(kNamespace, cluster.second, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, BalancerMostOverLoadShardHasMultipleZones) {
    // shard0 has chunks [MinKey, 1), [1, 2), [2, 3), [3, 4), [4, 5), so two chunks each
    // for zones "b" and "c". So [1, 2) is expected to be moved to shard1 in round 1.
    auto cluster = generateCluster({{5, 5 * kDefaultMaxChunkSizeBytes, false, {"a", "b", "c"}},
                                    {1, 1 * kDefaultMaxChunkSizeBytes, false, {"b"}},
                                    {1, 1 * kDefaultMaxChunkSizeBytes, false, {"c"}}});

    DistributionStatus distribution(kNamespace, cluster.second, ZoneInfo());
    ASSERT_OK(distribution.zoneInfo().addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 1), "a")));
    ASSERT_OK(
        distribution.zoneInfo().addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 3), "b")));
    ASSERT_OK(
        distribution.zoneInfo().addRangeToZone(ZoneRange(BSON("x" << 3), BSON("x" << 5), "c")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][1].getMin(), migrations[0].minKey);
}

TEST(BalancerPolicy, BalancerMostOverLoadShardHasMultipleZonesSkipZoneWithShardInUse) {
    // shard0 has chunks [MinKey, 1), [1, 2), [2, 3), [3, 4), [4, 5), so two chunks each
    // for zones "b" and "c". So [3, 4) is expected to be moved to shard2 because shard1 is
    // in use.
    auto cluster = generateCluster({{5, 5 * kDefaultMaxChunkSizeBytes, false, {"a", "b", "c"}},
                                    {1, 1 * kDefaultMaxChunkSizeBytes, false, {"b"}},
                                    {1, 1 * kDefaultMaxChunkSizeBytes, false, {"c"}}});

    DistributionStatus distribution(kNamespace, cluster.second, ZoneInfo());
    ASSERT_OK(distribution.zoneInfo().addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 1), "a")));
    ASSERT_OK(
        distribution.zoneInfo().addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 3), "b")));
    ASSERT_OK(
        distribution.zoneInfo().addRangeToZone(ZoneRange(BSON("x" << 3), BSON("x" << 5), "c")));

    auto availableShards = getAllShardIds(cluster.first);
    availableShards.erase(getShardId(1));
    const auto [migrations, reason] =
        BalancerPolicy::balance(cluster.first,
                                distribution,
                                buildDataSizeInfoForBalancingFromShardStats(cluster.first),
                                &availableShards,
                                false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][3].getMin(), migrations[0].minKey);
}

TEST(BalancerPolicy, BalancerFixesIncorrectZonesInOtherwiseBalancedClusterParallel) {
    // Chunks are balanced across shards, but there are wrong zones, which need to be fixed
    auto cluster = generateCluster({{5, 3 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {5, 3 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                    {5, 3 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet},
                                    {5, 3 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet}});

    DistributionStatus distribution(kNamespace, cluster.second, ZoneInfo());
    ASSERT_OK(distribution.zoneInfo().addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 20), "a")));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);

    ASSERT_EQ(getShardId(3), migrations[1].from);
    ASSERT_EQ(getShardId(1), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(3)][0].getMin(), migrations[1].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerHandlesNoShardsWithZone) {
    auto cluster =
        generateCluster({{2, 5 * kDefaultMaxChunkSizeBytes}, {2, 5 * kDefaultMaxChunkSizeBytes}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 7), "NonExistentZone")));

    DistributionStatus distribution(kNamespace, cluster.second, std::move(zoneInfo));

    ASSERT(balanceChunks(cluster.first, distribution, false, false).first.empty());
}

TEST(DistributionStatus, AddZoneRangeOverlap) {
    ZoneInfo zInfo;

    // Note that there is gap between 10 and 20 for which there is no zone
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "a")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 20), BSON("x" << 30), "b")));

    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 2), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << -1), BSON("x" << 5), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 9), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 12), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 25), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 19), BSON("x" << 21), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << -1), BSON("x" << 32), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 25), kShardKeyPattern.globalMax(), "d")));
}

TEST(DistributionStatus, ChunkZonesSelectorWithRegularKeys) {
    ZoneInfo zInfo;

    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "a")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 10), BSON("x" << 20), "b")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 20), BSON("x" << 30), "c")));

    DistributionStatus d(kNamespace, ShardToChunksMap{}, std::move(zInfo));

    {
        ChunkType chunk;
        chunk.setMin(kShardKeyPattern.globalMin());
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
        chunk.setMax(kShardKeyPattern.globalMax());
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 40));
        chunk.setMax(kShardKeyPattern.globalMax());
        ASSERT_EQUALS("", d.getZoneForChunk(chunk));
    }
}

TEST(DistributionStatus, ChunkZonesSelectorWithMinMaxKeys) {
    ZoneInfo zInfo;

    ASSERT_OK(
        zInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << -100), "a")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << -10), BSON("x" << 10), "b")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 100), kShardKeyPattern.globalMax(), "c")));

    DistributionStatus d(kNamespace, ShardToChunksMap{}, std::move(zInfo));

    {
        ChunkType chunk;
        chunk.setMin(kShardKeyPattern.globalMin());
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
        chunk.setMax(kShardKeyPattern.globalMax());
        ASSERT_EQUALS("c", d.getZoneForChunk(chunk));
    }
}

}  // namespace
}  // namespace mongo
